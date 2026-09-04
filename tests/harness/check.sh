#!/usr/bin/env bash
# Boots one contained headless Umbriel per check in checks/, runs the check, kills everything it spawned, and asserts
# that instance exited cleanly. One instance per check is what makes a failure local: a check starts from the default
# config with no windows, no overview, and workspace 1 focused, so it asserts behaviour instead of maintaining hygiene
# for whatever runs next. Boot plus teardown measures about 80ms, under 4% of the suite, and it buys back the
# config-restore reloads and window-drain loops that shared-instance checks had to carry.
# Containment matters. A stock Umbriel start runs its built-in autostarts, and `dbus-update-activation-environment --systemd` would repoint the *caller's* session-wide WAYLAND_DISPLAY and UMBRIEL_SOCKET at this throwaway instance. Unsetting DBUS_SESSION_BUS_ADDRESS makes both autostarts fail harmlessly.
# Usage: check.sh <path-to-umbriel-binary> [name-fragment ...] [-j N|--jobs N] [-v|--verbose] [-l|--list]
# Each name fragment selects every check whose name contains it, so several fragments run several checks. Without a
# fragment the whole suite runs. A failing check keeps its runtime directory (compositor and client logs) and prints it.
# Checks are independent instances, so they run several at a time. `-j` or CHECK_JOBS sets how many; the default stays
# well under the core count because a check that asserts animation timing is the first thing an overloaded box breaks.
# Reporting order stays the declaration order regardless of which check finishes first.

set -euo pipefail

BINARY=${1:?usage: check.sh <umbriel-binary> [name-fragment ...] [-v] [-l]}
shift

FILTERS=()
VERBOSE=${CHECK_VERBOSE:-0}
LIST_ONLY=0
JOBS=${CHECK_JOBS:-0}
expect_jobs=0
for arg in "$@"; do
  if ((expect_jobs)); then
    JOBS=$arg
    expect_jobs=0
    continue
  fi
  case $arg in
    -v | --verbose) VERBOSE=1 ;;
    -l | --list) LIST_ONLY=1 ;;
    -j | --jobs) expect_jobs=1 ;;
    -j*) JOBS=${arg#-j} ;;
    --jobs=*) JOBS=${arg#--jobs=} ;;
    # An empty argument is no fragment at all, not a fragment that matches everything.
    '') ;;
    -*)
      echo "check: unknown option '$arg'" >&2
      exit 2
      ;;
    *) FILTERS+=("$arg") ;;
  esac
done
if ((expect_jobs)); then
  echo "check: -j needs a worker count" >&2
  exit 2
fi
if [[ ! $JOBS =~ ^[0-9]+$ ]]; then
  echo "check: worker count must be a non-negative integer, got '$JOBS'" >&2
  exit 2
fi
if ((JOBS == 0)); then
  cores=$(nproc 2>/dev/null || echo 1)
  JOBS=$((cores < 8 ? cores : 8))
fi

HARNESS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# A check that never returns would otherwise hang the suite with no output. The
# cap is per check and generous: the slowest checks drive two-second animations.
CHECK_TIMEOUT=${CHECK_TIMEOUT:-120}

# Colour only for a terminal, so piped output and CI logs stay plain text.
if [[ -t 1 && -z ${NO_COLOR:-} && ${TERM:-dumb} != dumb ]]; then
  TTY=1
  C_OFF=$'\e[0m'
  C_DIM=$'\e[2m'
  C_BOLD=$'\e[1m'
  C_PASS=$'\e[32m'
  C_FAIL=$'\e[31m'
  C_RUN=$'\e[33m'
else
  TTY=0
  C_OFF='' C_DIM='' C_BOLD='' C_PASS='' C_FAIL='' C_RUN=''
fi
readonly NAME_WIDTH=34
COLUMNS_MAX=${COLUMNS:-100}
[[ $COLUMNS_MAX -lt 60 ]] && COLUMNS_MAX=60

all_checks() {
  local check
  for check in "$HARNESS_DIR"/checks/*.sh; do
    basename "$check" .sh
  done
}

selects() {
  local name=$1 filter
  ((${#FILTERS[@]} == 0)) && return 0
  for filter in "${FILTERS[@]}"; do
    [[ $name == *"$filter"* ]] && return 0
  done
  return 1
}

if ((LIST_ONLY)); then
  while read -r name; do
    selects "$name" && echo "$name"
  done <<< "$(all_checks)"
  exit 0
fi

# Select before booting anything: an unmatched fragment is a typo, and reporting
# it costs nothing when no compositor is running yet.
SELECTED=()
while read -r name; do
  selects "$name" && SELECTED+=("$name")
done <<< "$(all_checks)"
TOTAL=$(all_checks | wc -l)
if ((${#SELECTED[@]} == 0)); then
  echo "check: no checks matched ${FILTERS[*]}" >&2
  echo "check: available checks:" >&2
  all_checks | sed 's/^/  /' >&2
  exit 1
fi

if [[ ! -x $BINARY ]]; then
  echo "check: '$BINARY' is not executable" >&2
  exit 1
fi
BINARY=$(realpath "$BINARY")
BINARY_DIR=$(dirname "$BINARY")

# Checks use helper clients built alongside the selected compositor. They land in
# the build directory's `tests` subdir, where their Meson definitions live.
# Keeping this resolution here makes every build mode consistent without each
# recipe having to export a matching set of paths.
CLIENT_DIR=$BINARY_DIR/tests
export UMBRIEL_POINTER_CLIENT="$CLIENT_DIR/pointer-client"
export UMBRIEL_KEYBOARD_KEYMAP_CLIENT="$CLIENT_DIR/keyboard-keymap-client"
export UMBRIEL_INPUT_METHOD_CLIENT="$CLIENT_DIR/input-method-client"
export UMBRIEL_DRAG_CLIENT="$CLIENT_DIR/drag-client"
export UMBRIEL_LAYER_CLIENT="$CLIENT_DIR/layer-client"
export UMBRIEL_GLOBAL_CLIENT="$CLIENT_DIR/global-client"
export UMBRIEL_DATA_CONTROL_CLIENT="$CLIENT_DIR/data-control-client"
export UMBRIEL_WORKSPACE_CLIENT="$CLIENT_DIR/workspace-client"
export UMBRIEL_FOREIGN_TOPLEVEL_CLIENT="$CLIENT_DIR/foreign-toplevel-client"
export UMBRIEL_UNMAP_CLIENT="$CLIENT_DIR/unmap-client"
export UMBRIEL_POPUP_CLIENT="$CLIENT_DIR/popup-client"
export UMBRIEL_IDLE_INHIBIT_CLIENT="$CLIENT_DIR/idle-inhibit-client"
export UMBRIEL_LOCK_CLIENT="$CLIENT_DIR/lock-client"
export UMBRIEL_SUBSURFACE_CLIENT="$CLIENT_DIR/subsurface-client"
export UMBRIEL_FRACTIONAL_CLIENT="$CLIENT_DIR/fractional-client"
export UMBRIEL_SECURITY_CONTEXT_CLIENT="$CLIENT_DIR/security-context-client"
export UMBRIEL=$BINARY

# Live instance state. The EXIT trap reaches for these, so they stay declared
# even before the first check boots.
RUNTIME_DIR=
SERVER_PID=
INSTANCE_PGID=
CHECK_PGID=
IPC_CLIENT_PID=
KEPT_DIRS=()

now_us() {
  # EPOCHREALTIME is "seconds.microseconds" with a locale-dependent radix, so
  # dropping the separator yields plain microseconds without spawning a process.
  local stamp=${EPOCHREALTIME:-}
  if [[ -z $stamp ]]; then
    date +%s%6N
    return
  fi
  echo "${stamp/[.,]/}"
}

elapsed() {
  local us=$(($(now_us) - $1))
  printf '%d.%02ds' "$((us / 1000000))" "$((us % 1000000 / 10000))"
}

# The harness's own process group. Signalling it would take the suite down, and
# with several workers in flight it would take their instances with it, so it is
# the one group id that is never a valid answer below.
OWN_PGID=$(ps -o pgid= -p $$ 2>/dev/null | tr -d ' ' || true)

# The group id a setsid'd child reports for itself, or empty when it never got
# far enough to have one. Asking `ps` for the child's group instead loses a race
# it cannot win: until setsid() takes effect the child is still in the harness's
# group, and a kill aimed at that answer is a suicide.
child_pgid() {
  local file=$1 pid=$2 waited=0 value=
  while ((waited < 500)); do
    if [[ -s $file ]]; then
      value=$(< "$file")
      break
    fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.002
    waited=$((waited + 1))
  done
  if [[ -z $value || $value == "$OWN_PGID" ]] || ((value <= 1)); then
    return 0
  fi
  printf '%s\n' "$value"
}

# Everything a check spawns lives in the check's own process group, so one
# signal reaches clients the check lost track of. Killing by group is what lets
# checks stop bookkeeping pids: capturing `$!` from a shell function yields the
# forked subshell, not the client, and that mistake used to leak mapped windows
# into every later check.
kill_check_group() {
  [[ -z $CHECK_PGID ]] && return 0
  if [[ $CHECK_PGID != "$OWN_PGID" ]] && ((CHECK_PGID > 1)); then
    kill -TERM -- "-$CHECK_PGID" 2>/dev/null || true
    kill -KILL -- "-$CHECK_PGID" 2>/dev/null || true
  fi
  CHECK_PGID=
}

# Kills the instance and everything it forked. The compositor runs in its own
# session, so processes it spawned itself (autostarts, `msg spawn:`) sit in the
# instance's process group rather than the check's, and reaping that group is
# the only way they do not outlive the run.
kill_instance() {
  if [[ -n $IPC_CLIENT_PID ]] && kill -0 "$IPC_CLIENT_PID" 2>/dev/null; then
    kill -KILL "$IPC_CLIENT_PID" 2>/dev/null || true
    wait "$IPC_CLIENT_PID" 2>/dev/null || true
  fi
  IPC_CLIENT_PID=
  if [[ -n $SERVER_PID ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -KILL "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  SERVER_PID=
  reap_instance_group
}

reap_instance_group() {
  [[ -z $INSTANCE_PGID ]] && return 0
  if [[ $INSTANCE_PGID != "$OWN_PGID" ]] && ((INSTANCE_PGID > 1)); then
    kill -KILL -- "-$INSTANCE_PGID" 2>/dev/null || true
  fi
  INSTANCE_PGID=
}

cleanup() {
  kill_check_group
  kill_instance
  # A failed check is worth debugging, and its evidence (compositor log,
  # per-client logs, config) lives in its runtime directory. The summary keeps
  # those and points at them; anything still live here is from an aborted run.
  [[ -n $RUNTIME_DIR && -d $RUNTIME_DIR ]] && rm -rf "$RUNTIME_DIR"
  RUNTIME_DIR=
}

header() {
  printf '%s\n' "${C_BOLD}check${C_OFF} ${C_DIM}·${C_OFF} $BINARY"
  local scope="${#SELECTED[@]} of $TOTAL checks"
  ((${#FILTERS[@]} > 0)) && scope+=" (filter: ${FILTERS[*]})"
  local pool="one compositor instance each"
  ((JOBS > 1)) && pool+=", $JOBS at a time"
  printf '%s\n' "       ${C_DIM}·${C_OFF} $scope, $pool"
  printf '\n'
}

start_row() {
  ((TTY)) || return 0
  printf '  %sRUN %s %s%s%s' "$C_RUN" "$C_OFF" "$C_DIM" "$1" "$C_OFF"
}

# Pass detail is context, not a finding: one dimmed line, elided to the terminal
# width. Failure detail is the finding itself and is never trimmed.
detail() {
  local status=$1 text=$2
  [[ -z $text ]] && return 0
  if [[ $status == PASS ]] && ((!VERBOSE)); then
    local first=${text%%$'\n'*}
    local room=$((COLUMNS_MAX - 7))
    if [[ ${#first} -gt $room || $first != "$text" ]]; then
      first=${first:0:room}…
    fi
    printf '%s\n' "       ${C_DIM}${first}${C_OFF}"
    return 0
  fi
  local marker="${C_DIM}│${C_OFF}"
  [[ $status == FAIL ]] && marker="${C_FAIL}│${C_OFF}"
  while IFS= read -r line; do
    printf '%s\n' "     $marker $line"
  done <<< "$text"
}

row() {
  local status=$1 name=$2 duration=$3 text=${4:-}
  local colour=$C_PASS
  [[ $status == FAIL ]] && colour="${C_FAIL}${C_BOLD}"
  ((TTY)) && printf '\r\e[2K'
  printf '  %s%s%s %-*s %s%7s%s\n' "$colour" "$status" "$C_OFF" "$NAME_WIDTH" "$name" "$C_DIM" "$duration" "$C_OFF"
  detail "$status" "$text"
}

# No autostart, no xwayland, no cheatsheet: a check wants a bare compositor, and
# each of those would spawn processes outside the container.
write_default_config() {
  cat > "$1" << 'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
EOF
}

# A check that needs a second monitor declares it in its header and the harness boots that instance accordingly.
# Everything else gets one output, which is what most geometry assertions are written against. A check that needs
# monitors to come and go uses `umbriel output-create` and `umbriel output-destroy` on top of what it declares here.
check_outputs() {
  local declared
  declared=$(sed -n '2,12p' "$HARNESS_DIR/checks/$1.sh" |
    sed -n 's/^# harness: outputs=\([0-9][0-9]*\).*/\1/p' | head -1)
  [[ -z $declared ]] && declared=1
  echo "$declared"
}

# Boots an instance and exports the environment a check runs against. On failure
# it sets BOOT_ERROR and leaves the runtime directory for the caller to keep.
start_instance() {
  local outputs=$1
  # sockaddr_un caps paths at 108 bytes and the compositor appends
  # "/umbriel-wayland-0.sock" (23) to XDG_RUNTIME_DIR, so keep the root short. A
  # long path makes wl_display_add_socket fail and the boot abort.
  RUNTIME_DIR=$(mktemp -d /tmp/umv.XXXXXXXX)
  local log=$RUNTIME_DIR/compositor.log
  local config=$RUNTIME_DIR/config.toml
  local socket=$RUNTIME_DIR/umbriel-wayland-0.sock
  write_default_config "$config"

  # setsid puts the compositor in a session of its own, so anything it forks
  # (an autostart, a keybind `spawn:`) is reachable as one process group at
  # teardown instead of joining the harness's own group where it cannot be
  # signalled. The wrapper reports the group id it leads and then execs the
  # compositor in place, so SERVER_PID stays the compositor.
  local pgid_file=$RUNTIME_DIR/instance.pgid
  setsid env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    WLR_BACKENDS=headless \
    WLR_LIBINPUT_NO_DEVICES=1 \
    WLR_HEADLESS_OUTPUTS="$outputs" \
    bash -c 'echo $$ > "$1"; shift; exec "$@"' _ "$pgid_file" \
    "$BINARY" -c "$config" > "$log" 2>&1 &
  SERVER_PID=$!
  INSTANCE_PGID=$(child_pgid "$pgid_file" "$SERVER_PID")

  # Boot lands in tens of milliseconds, and this runs once per check, so poll
  # tightly rather than in quarter-second steps.
  local waited_ms=0
  while [[ ! -S $socket ]]; do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      wait "$SERVER_PID" 2>/dev/null || true
      SERVER_PID=
      BOOT_ERROR="compositor died during boot"$'\n'"$(< "$log")"
      return 1
    fi
    if ((waited_ms >= 10000)); then
      BOOT_ERROR="IPC socket never appeared within 10s"$'\n'"$(< "$log")"
      return 1
    fi
    sleep 0.005
    waited_ms=$((waited_ms + 5))
  done

  export UMBRIEL_SOCKET=$socket
  export UMBRIEL_RUNTIME_DIR=$RUNTIME_DIR
  export UMBRIEL_LOG=$log
  export UMBRIEL_CONFIG=$config
  return 0
}

# Clean shutdown is itself an assertion, and now every check makes it: a listener still attached to a wlroots object at teardown trips an assert and the process dies on SIGABRT (exit 134) after having already logged "shutting down". One incomplete IPC connection stays registered through teardown. Completed connections were exercised by the check itself; both lifecycle paths must leave no event source or descriptor behind.
attach_idle_ipc_client() {
  local ready=$RUNTIME_DIR/ipc-idle-ready
  # A refused connection is a legitimate outcome here (a check may have taken
  # its own instance down), so the client's traceback belongs in the runtime
  # directory next to the compositor log, not in the suite's output.
  python3 - "$UMBRIEL_SOCKET" "$ready" > "$RUNTIME_DIR/ipc-idle.log" 2>&1 << 'PY' &
import pathlib
import socket
import sys
import time

client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(sys.argv[1])
client.sendall(b"{")
pathlib.Path(sys.argv[2]).touch()
time.sleep(300)
PY
  IPC_CLIENT_PID=$!
  local waited=0
  while [[ ! -f $ready ]]; do
    # A refused connection ends the client immediately, and there is nothing to
    # wait for once it is gone.
    if ! kill -0 "$IPC_CLIENT_PID" 2>/dev/null; then
      return 1
    fi
    if ((waited >= 200)); then
      return 1
    fi
    sleep 0.01
    waited=$((waited + 1))
  done
  return 0
}

# Terminates the instance and reports whether it went down cleanly. Sets
# STOP_ERROR when it did not. The exit status is the stronger finding, so a
# failed idle-client attach is only reported when the compositor still managed
# to exit cleanly. A check that quits the compositor itself (session-quit) is
# expected: bash reaps a background child as soon as it dies, so the instance is
# simply gone by now and is judged by the status bash kept for it.
stop_instance() {
  STOP_ERROR=
  local attached=1
  if kill -0 "$SERVER_PID" 2>/dev/null; then
    attach_idle_ipc_client || attached=0
  fi

  local status=0
  kill -TERM "$SERVER_PID" 2>/dev/null || true
  # Bash announces an async job that died from a signal on its own stderr when
  # it reaps one, which is this harness's own report to make.
  { wait "$SERVER_PID" || status=$?; } 2>/dev/null
  SERVER_PID=
  if [[ -n $IPC_CLIENT_PID ]] && kill -0 "$IPC_CLIENT_PID" 2>/dev/null; then
    kill -KILL "$IPC_CLIENT_PID" 2>/dev/null || true
    wait "$IPC_CLIENT_PID" 2>/dev/null || true
  fi
  IPC_CLIENT_PID=
  reap_instance_group

  if [[ $status -ne 0 ]]; then
    STOP_ERROR="compositor exited with status $status at teardown, expected 0"$'\n'"$(tail -5 "$UMBRIEL_LOG")"
    return 1
  fi
  if ((!attached)); then
    STOP_ERROR="idle IPC client never connected, so teardown ran without one"
    return 1
  fi
  return 0
}

# Runs the check body in its own process group so the harness can reap whatever
# it spawned. Output goes to a file because a command substitution cannot own a
# background job.
# The body runs pointed at its own instance, not at the session that started the
# suite. This is not a convenience: only IPC subcommands honour UMBRIEL_SOCKET,
# while `umbriel outputs` and every helper client are Wayland clients that
# resolve XDG_RUNTIME_DIR and WAYLAND_DISPLAY, so an inherited session
# environment silently points them at the developer's live compositor.
run_check_body() {
  local name=$1 output_file=$2
  local pgid_file=$RUNTIME_DIR/check.pgid
  setsid env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    WAYLAND_DISPLAY=wayland-0 \
    bash -c 'echo $$ > "$1"; shift; exec "$@"' _ "$pgid_file" \
    timeout -k 5 "$CHECK_TIMEOUT" bash "$HARNESS_DIR/checks/$name.sh" > "$output_file" 2>&1 &
  local body_pid=$!
  CHECK_PGID=$(child_pgid "$pgid_file" "$body_pid")
  local status=0
  # An interrupted suite kills the body group, and bash would announce that
  # killed job on its own stderr in the middle of the report.
  { wait "$body_pid" || status=$?; } 2>/dev/null
  kill_check_group
  return "$status"
}

# One check, start to verdict: its own instance, its own process groups, its own
# runtime directory. This runs in a background subshell, so the instance state
# above is private to it and its own EXIT trap reaps the instance even when the
# pool kills the worker. The verdict travels back through files because a
# subshell cannot assign to its parent.
run_one() {
  local name=$1 prefix=$2
  trap 'cleanup; exit 143' TERM INT
  trap cleanup EXIT

  local check_start
  check_start=$(now_us)

  BOOT_ERROR=
  if ! start_instance "$(check_outputs "$name")"; then
    publish "$prefix" 1 "$check_start" "$BOOT_ERROR"
    return 0
  fi

  local body_status=0 output=
  run_check_body "$name" "$prefix.body" || body_status=$?
  output=$(< "$prefix.body")
  rm -f "$prefix.body"
  if ((body_status == 124 || body_status == 137)); then
    output="check exceeded ${CHECK_TIMEOUT}s and was killed"$'\n'"$output"
  fi

  local stop_status=0
  stop_instance || stop_status=$?
  if ((body_status == 0 && stop_status != 0)); then
    body_status=$stop_status
    output=${output:+$output$'\n'}$STOP_ERROR
  fi

  publish "$prefix" "$body_status" "$check_start" "$output"
  return 0
}

# Records a worker's verdict. The status file is written last: the pool treats
# its existence as the completion signal, so it must not appear before the
# detail and duration it describes. A failing check keeps its runtime directory
# as evidence, which means clearing RUNTIME_DIR so the EXIT trap spares it.
publish() {
  local prefix=$1 status=$2 start=$3 text=$4
  printf '%s' "$text" > "$prefix.out"
  elapsed "$start" > "$prefix.time"
  if [[ -n $RUNTIME_DIR ]]; then
    if ((status == 0)); then
      rm -rf "$RUNTIME_DIR"
    else
      printf '%s\n' "$RUNTIME_DIR" > "$prefix.dir"
    fi
    RUNTIME_DIR=
  fi
  printf '%s\n' "$status" > "$prefix.status"
}

report_one() {
  local name=$1 prefix=$RESULT_DIR/$name
  local status=1 text= duration=0.00s dir=
  [[ -f $prefix.status ]] && status=$(< "$prefix.status")
  [[ -f $prefix.out ]] && text=$(< "$prefix.out")
  [[ -f $prefix.time ]] && duration=$(< "$prefix.time")
  if ((status == 0)); then
    row PASS "$name" "$duration" "$text"
    passed=$((passed + 1))
  else
    row FAIL "$name" "$duration" "$text"
    FAILED_NAMES+=("$name")
    [[ -f $prefix.dir ]] && dir=$(< "$prefix.dir")
    KEPT_DIRS+=("$dir")
  fi
  rm -f "$prefix.status" "$prefix.out" "$prefix.time" "$prefix.dir"
}

# Dispatched but without a verdict yet. This is what the pool bounds, and it
# counts finished-not-yet-reported workers as free: reporting is ordered by
# declaration so the output is stable, while execution is not.
running_count() {
  local index count=0
  for ((index = REPORTED; index < DISPATCHED; index++)); do
    [[ -f $RESULT_DIR/${SELECTED[index]}.status ]] || count=$((count + 1))
  done
  echo "$count"
}

# A worker that died without publishing (SIGKILL, or a bash failure inside
# run_one) would otherwise leave the pool waiting on a child that no longer
# exists, so give it a verdict of its own.
fail_unpublished() {
  local index name
  for ((index = REPORTED; index < DISPATCHED; index++)); do
    name=${SELECTED[index]}
    [[ -f $RESULT_DIR/$name.status ]] && continue
    printf '%s' "worker exited without a verdict" > "$RESULT_DIR/$name.out"
    printf '0.00s' > "$RESULT_DIR/$name.time"
    printf '1\n' > "$RESULT_DIR/$name.status"
  done
}

terminate_workers() {
  local name
  for name in "${!WORKER_PID[@]}"; do
    kill -TERM "${WORKER_PID[$name]}" 2>/dev/null || true
  done
  for name in "${!WORKER_PID[@]}"; do
    wait "${WORKER_PID[$name]}" 2>/dev/null || true
  done
  WORKER_PID=()
}

suite_cleanup() {
  terminate_workers
  cleanup
  [[ -n $RESULT_DIR && -d $RESULT_DIR ]] && rm -rf "$RESULT_DIR"
  RESULT_DIR=
}
trap suite_cleanup EXIT

RESULT_DIR=$(mktemp -d /tmp/umv-results.XXXXXXXX)
declare -A WORKER_PID=()
DISPATCHED=0
REPORTED=0

header
suite_start=$(now_us)
passed=0
FAILED_NAMES=()

while ((REPORTED < ${#SELECTED[@]})); do
  while ((DISPATCHED < ${#SELECTED[@]} && $(running_count) < JOBS)); do
    name=${SELECTED[DISPATCHED]}
    # With one worker the live row is the progress indicator. With more it would
    # be a lie, because several checks are in flight at once.
    ((JOBS == 1)) && start_row "$name"
    run_one "$name" "$RESULT_DIR/$name" &
    WORKER_PID[$name]=$!
    DISPATCHED=$((DISPATCHED + 1))
  done

  while ((REPORTED < DISPATCHED)) && [[ -f $RESULT_DIR/${SELECTED[REPORTED]}.status ]]; do
    name=${SELECTED[REPORTED]}
    wait "${WORKER_PID[$name]}" 2>/dev/null || true
    unset "WORKER_PID[$name]"
    report_one "$name"
    REPORTED=$((REPORTED + 1))
  done
  ((REPORTED >= ${#SELECTED[@]})) && break

  # Nothing new to report: block until some worker exits rather than polling.
  # Each call reaps at most one child, so this always makes progress.
  wait_status=0
  wait -n 2>/dev/null || wait_status=$?
  ((wait_status == 127)) && fail_unpublished
done

failed=${#FAILED_NAMES[@]}
total_time=$(elapsed "$suite_start")
printf '\n'
if ((failed > 0)); then
  printf '%s\n' "  ${C_FAIL}${C_BOLD}${failed} failed${C_OFF} ${C_DIM}·${C_OFF} $passed passed ${C_DIM}·${C_OFF} ${C_DIM}${total_time}${C_OFF}"
  for index in "${!FAILED_NAMES[@]}"; do
    printf '%s\n' "    ${C_FAIL}·${C_OFF} ${FAILED_NAMES[index]} ${C_DIM}(${KEPT_DIRS[index]})${C_OFF}"
  done
  printf '%s\n' "  ${C_DIM}each directory holds that check's compositor.log, config, and client logs${C_OFF}"
  exit 1
fi
printf '%s\n' "  ${C_PASS}${C_BOLD}${passed} passed${C_OFF} ${C_DIM}·${C_OFF} ${C_DIM}${total_time}${C_OFF}"
