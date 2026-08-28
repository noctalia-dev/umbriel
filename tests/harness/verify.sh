#!/usr/bin/env bash
# Boots one contained headless Umbriel per check in checks/, runs the check, kills everything it spawned, and asserts
# that instance exited cleanly. One instance per check is what makes a failure local: a check starts from the default
# config with no windows, no overview, and workspace 1 focused, so it asserts behaviour instead of maintaining hygiene
# for whatever runs next. Boot plus teardown measures about 80ms, under 4% of the suite, and it buys back the
# config-restore reloads and window-drain loops that shared-instance checks had to carry.
# Containment matters. A stock Umbriel start runs its built-in autostarts, and `dbus-update-activation-environment --systemd` would repoint the *caller's* session-wide WAYLAND_DISPLAY and UMBRIEL_SOCKET at this throwaway instance. Unsetting DBUS_SESSION_BUS_ADDRESS makes both autostarts fail harmlessly.
# Usage: verify.sh <path-to-umbriel-binary> [name-fragment ...] [-v|--verbose] [-l|--list]
# Each name fragment selects every check whose name contains it, so several fragments run several checks. Without a
# fragment the whole suite runs. A failing check keeps its runtime directory (compositor and client logs) and prints it.

set -euo pipefail

BINARY=${1:?usage: verify.sh <umbriel-binary> [name-fragment ...] [-v] [-l]}
shift

FILTERS=()
VERBOSE=${VERIFY_VERBOSE:-0}
LIST_ONLY=0
for arg in "$@"; do
  case $arg in
    -v | --verbose) VERBOSE=1 ;;
    -l | --list) LIST_ONLY=1 ;;
    # `just verify debug` forwards an empty filter; treat it as "no filter".
    '') ;;
    -*)
      echo "verify: unknown option '$arg'" >&2
      exit 2
      ;;
    *) FILTERS+=("$arg") ;;
  esac
done

HARNESS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# A check that never returns would otherwise hang the suite with no output. The
# cap is per check and generous: the slowest checks drive two-second animations.
CHECK_TIMEOUT=${VERIFY_TIMEOUT:-120}

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
  echo "verify: no checks matched ${FILTERS[*]}" >&2
  echo "verify: available checks:" >&2
  all_checks | sed 's/^/  /' >&2
  exit 1
fi

if [[ ! -x $BINARY ]]; then
  echo "verify: '$BINARY' is not executable" >&2
  exit 1
fi
BINARY=$(realpath "$BINARY")
BINARY_DIR=$(dirname "$BINARY")

# Checks use helper clients built alongside the selected compositor. Keeping
# this resolution here makes every build mode consistent without each recipe
# having to export a matching set of paths.
export UMBRIEL_POINTER_CLIENT="$BINARY_DIR/pointer-client"
export UMBRIEL_INPUT_METHOD_CLIENT="$BINARY_DIR/input-method-client"
export UMBRIEL_DRAG_CLIENT="$BINARY_DIR/drag-client"
export UMBRIEL_LAYER_CLIENT="$BINARY_DIR/layer-client"
export UMBRIEL_GLOBAL_CLIENT="$BINARY_DIR/global-client"
export UMBRIEL_WORKSPACE_CLIENT="$BINARY_DIR/workspace-client"
export UMBRIEL_FOREIGN_TOPLEVEL_CLIENT="$BINARY_DIR/foreign-toplevel-client"
export UMBRIEL_UNMAP_CLIENT="$BINARY_DIR/unmap-client"
export UMBRIEL_POPUP_CLIENT="$BINARY_DIR/popup-client"
export UMBRIEL_IDLE_INHIBIT_CLIENT="$BINARY_DIR/idle-inhibit-client"
export UMBRIEL_SUBSURFACE_CLIENT="$BINARY_DIR/subsurface-client"
export UMBRIEL_FRACTIONAL_CLIENT="$BINARY_DIR/fractional-client"
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

# Everything a check spawns lives in the check's own process group, so one
# signal reaches clients the check lost track of. Killing by group is what lets
# checks stop bookkeeping pids: capturing `$!` from a shell function yields the
# forked subshell, not the client, and that mistake used to leak mapped windows
# into every later check.
kill_check_group() {
  [[ -z $CHECK_PGID ]] && return 0
  # Never signal our own group: that would take the harness down with it.
  if [[ $CHECK_PGID != "$$" && $CHECK_PGID -gt 1 ]]; then
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
  if [[ $INSTANCE_PGID != "$$" && $INSTANCE_PGID -gt 1 ]]; then
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
trap cleanup EXIT

header() {
  printf '%s\n' "${C_BOLD}verify${C_OFF} ${C_DIM}·${C_OFF} $BINARY"
  local scope="${#SELECTED[@]} of $TOTAL checks"
  ((${#FILTERS[@]} > 0)) && scope+=" (filter: ${FILTERS[*]})"
  printf '%s\n' "       ${C_DIM}·${C_OFF} $scope, one compositor instance each"
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
  # signalled. The backgrounded child is not a group leader, so setsid execs in
  # place and SERVER_PID stays the compositor and its group id.
  setsid env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    WLR_BACKENDS=headless \
    WLR_LIBINPUT_NO_DEVICES=1 \
    WLR_HEADLESS_OUTPUTS="$outputs" \
    "$BINARY" -c "$config" > "$log" 2>&1 &
  SERVER_PID=$!
  INSTANCE_PGID=$(ps -o pgid= -p "$SERVER_PID" 2>/dev/null | tr -d ' ' || true)
  [[ -z $INSTANCE_PGID ]] && INSTANCE_PGID=$SERVER_PID

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
# background job, and the group leader's pgid has to be read before the wait.
# The body runs pointed at its own instance, not at the session that started the
# suite. This is not a convenience: only IPC subcommands honour UMBRIEL_SOCKET,
# while `umbriel outputs` and every helper client are Wayland clients that
# resolve XDG_RUNTIME_DIR and WAYLAND_DISPLAY, so an inherited session
# environment silently points them at the developer's live compositor.
run_check_body() {
  local name=$1 output_file=$2
  setsid env -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    WAYLAND_DISPLAY=wayland-0 \
    timeout -k 5 "$CHECK_TIMEOUT" bash "$HARNESS_DIR/checks/$name.sh" > "$output_file" 2>&1 &
  local body_pid=$!
  CHECK_PGID=$(ps -o pgid= -p "$body_pid" 2>/dev/null | tr -d ' ' || true)
  [[ -z $CHECK_PGID ]] && CHECK_PGID=$body_pid
  local status=0
  wait "$body_pid" || status=$?
  kill_check_group
  return "$status"
}

header
suite_start=$(now_us)
passed=0
FAILED_NAMES=()

for name in "${SELECTED[@]}"; do
  start_row "$name"
  check_start=$(now_us)
  output_file=$(mktemp /tmp/umv-out.XXXXXXXX)

  BOOT_ERROR=
  if ! start_instance "$(check_outputs "$name")"; then
    row FAIL "$name" "$(elapsed "$check_start")" "$BOOT_ERROR"
    FAILED_NAMES+=("$name")
    KEPT_DIRS+=("$RUNTIME_DIR")
    RUNTIME_DIR=
    rm -f "$output_file"
    continue
  fi

  body_status=0
  run_check_body "$name" "$output_file" || body_status=$?
  output=$(< "$output_file")
  rm -f "$output_file"
  if ((body_status == 124 || body_status == 137)); then
    output="check exceeded ${CHECK_TIMEOUT}s and was killed"$'\n'"$output"
  fi

  stop_status=0
  stop_instance || stop_status=$?
  if ((body_status == 0 && stop_status != 0)); then
    body_status=$stop_status
    output=${output:+$output$'\n'}$STOP_ERROR
  fi

  if ((body_status == 0)); then
    row PASS "$name" "$(elapsed "$check_start")" "$output"
    passed=$((passed + 1))
    rm -rf "$RUNTIME_DIR"
  else
    row FAIL "$name" "$(elapsed "$check_start")" "$output"
    FAILED_NAMES+=("$name")
    KEPT_DIRS+=("$RUNTIME_DIR")
  fi
  RUNTIME_DIR=
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
