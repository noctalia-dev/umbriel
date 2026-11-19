#!/usr/bin/env bash
# Backend-boundary input verifies approval and capture handoff in a private compositor.
set -euo pipefail
trap 'echo "global shortcut check failed at line $LINENO: $BASH_COMMAND" >&2' ERR
umask 077
readonly BUILD=$(realpath "${1:?usage: global_shortcuts.sh BUILD_DIRECTORY}")
readonly RUNTIME=$(mktemp -d /tmp/umbriel-shortcuts-input.XXXXXXXX)
readonly CLIENT=$BUILD/tests/global-shortcuts-client
readonly COMPOSITOR=$BUILD/tests/input-capture-compositor
readonly UMBRIEL=$BUILD/umbriel
readonly REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PIDS=()
SUCCESS=0
cleanup() {
  local pid
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  if ((SUCCESS)) && [[ ${UMBRIEL_SHORTCUTS_KEEP_LOGS:-0} != 1 ]]; then
    rm -rf "$RUNTIME"
  else
    echo "global shortcut input logs retained in $RUNTIME"
  fi
}
trap cleanup EXIT
export XDG_RUNTIME_DIR=$RUNTIME XDG_CONFIG_HOME=$RUNTIME/config XDG_CACHE_HOME=$RUNTIME/cache
export XDG_STATE_HOME=$RUNTIME/state
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUNTIME/no-session-bus
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=$RUNTIME/no-system-bus
unset DISPLAY WAYLAND_SOCKET NOTIFY_SOCKET SYSTEMD_EXEC_PID
source "$REPO/tests/harness/lib.sh"
cat > "$RUNTIME/config.toml" <<'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
[animation]
enabled = false
[input.keyboard]
layout = "us"
EOF
"$CLIENT" seed "$RUNTIME/grant" org.test.App talk CTRL+m
for capability in 1 2; do
  "$CLIENT" seed-capture "$RUNTIME/capture-$capability" "org.test.Capture$capability" "$capability"
done
mkfifo "$RUNTIME/physical" "$RUNTIME/shortcuts" "$RUNTIME/capture"
exec {physical_fd}<> "$RUNTIME/physical"
exec {shortcut_fd}<> "$RUNTIME/shortcuts"
exec {capture_fd}<> "$RUNTIME/capture"
WAYLAND_DISPLAY=shortcuts-isolated-parent WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
  "$COMPOSITOR" "$RUNTIME/config.toml" <&"$physical_fd" > "$RUNTIME/compositor.log" 2>&1 &
PIDS+=("$!")
COMPOSITOR_PID=${PIDS[0]}
await_events "$RUNTIME/compositor.log" 'ready display=' 1
export WAYLAND_DISPLAY UMBRIEL_SOCKET
WAYLAND_DISPLAY=$(sed -n 's/^ready display=\([^ ]*\).*/\1/p' "$RUNTIME/compositor.log" | head -1)
UMBRIEL_SOCKET=$(sed -n 's/^ready .*socket=\([^ ]*\).*/\1/p' "$RUNTIME/compositor.log" | head -1)
"$CLIENT" <&"$shortcut_fd" > "$RUNTIME/shortcuts.log" 2>&1 &
PIDS+=("$!")
CAPTURE_PROTOCOL_VERSION=2 "$BUILD/tests/input-capture-client" <&"$capture_fd" > "$RUNTIME/capture.log" 2>&1 &
PIDS+=("$!")
"$BUILD/tests/seat-log-client" shortcut-physical-observer > "$RUNTIME/local.log" 2>&1 &
PIDS+=("$!")
await_events "$RUNTIME/local.log" keyboard-enter 1
await_events "$RUNTIME/shortcuts.log" ready 1
await_events "$RUNTIME/capture.log" ready 1
printf 'new org.test.App\nrestore %s\nadd talk CTRL+m\nbind\n' "$RUNTIME/grant" >&"$shortcut_fd"
await_events "$RUNTIME/shortcuts.log" 'bound .*status=0' 1
physical() { printf '%s\n' "$@" >&"$physical_fd"; }
capture() { printf '%s\n' "$@" >&"$capture_fd"; }
sync_bridge() {
  physical "mark $1"
  await_events "$RUNTIME/compositor.log" "mark $1$" 1
  printf 'sync %s\n' "$1" >&"$shortcut_fd"
  await_events "$RUNTIME/shortcuts.log" "sync $1$" 1
}
expect_count() {
  local actual
  actual=$(events "$RUNTIME/shortcuts.log" "$1")
  if ((actual != $2)); then
    echo "expected $2 '$1', got $actual"
    cat "$RUNTIME/shortcuts.log"
    exit 1
  fi
}

send() { printf '%s\n' "$@" >&"$shortcut_fd"; }
next_session=1
new_session() {
  SESSION=$next_session
  next_session=$((next_session + 1))
  send "new $1" "credential $RUNTIME/credential-$SESSION"
}
request() {
  new_session "$1"
  send 'add requested F17' bind
  sync_bridge "request-$SESSION"
}
status_serial=0
review_status() {
  status_serial=$((status_serial + 1))
  physical "shortcut-status $status_serial"
  await_events "$RUNTIME/compositor.log" "shortcut-status tag=$status_serial " 1
  read -r VISIBLE PAGE PAGES < <(sed -n -E \
    "s/^shortcut-status tag=$status_serial visible=([^ ]+) page=([0-9]+) pages=([0-9]+)$/\\1 \\2 \\3/p" \
    "$RUNTIME/compositor.log" | tail -1)
}
bound() { await_events "$RUNTIME/shortcuts.log" "bound session=$1 .*status=$2" 1; }
snapshot() {
  if [[ ${UMBRIEL_SHORTCUTS_SCREENSHOTS:-0} == 1 ]]; then
    "$UMBRIEL" settle
    grim "$RUNTIME/$1.png"
  fi
}

# A held Enter and virtual keys cannot decide a new application's approval.
physical 'press 28'
sync_bridge held-enter
request org.test.Fresh
review_status
[[ $VISIBLE == true && $PAGE == 1 && $PAGES == 1 ]]
snapshot approval-first
"$BUILD/tests/pointer-client" 1280 720 keyboard-only tap 28 tap 1
physical 'press 28'
sync_bridge blocked-enter
expect_count "bound session=$SESSION " 0
review_status
[[ $VISIBLE == true && $PAGE == 1 ]]
physical 'release 28'
await_events "$RUNTIME/local.log" 'keyboard-key code=28 state=released' 1
[[ $(events "$RUNTIME/local.log" 'keyboard-key code=28 state=pressed') == \
   $(events "$RUNTIME/local.log" 'keyboard-key code=28 state=released') ]]
physical 'tap 28'
bound "$SESSION" 0
[[ -s $RUNTIME/credential-$SESSION ]]
send close

# Each page needs a distinct physical press, and resizing restarts review.
new_session org.test.Pages
send 'add-many 6 800' bind
sync_bridge paginated
review_status
[[ $VISIBLE == true && $PAGES -gt 1 && $PAGE == 1 ]]
physical 'press 28'
sync_bridge advanced
review_status
[[ $PAGE == 2 ]]
physical 'press 28'
"$BUILD/tests/pointer-client" 1280 720 keyboard-only tap 28
sync_bridge repeated
review_status
[[ $PAGE == 2 ]]
expect_count "bound session=$SESSION " 0
physical 'release 28' 'output-mode 700 500'
sync_bridge resized
"$UMBRIEL" settle
review_status
[[ $VISIBLE == true && $PAGE == 1 && $PAGES -gt 1 ]]
while ((PAGE < PAGES)); do
  physical 'tap 28'
  sync_bridge "page-$PAGE"
  review_status
done
expect_count "bound session=$SESSION " 0
snapshot approval-final
physical 'tap 28'
bound "$SESSION" 0
jq -e '[.grants[] | select(.application == "org.test.Pages") | .approved | length] == [6]' \
  "$XDG_STATE_HOME/umbriel/global-shortcuts.json" > /dev/null
send close
physical 'output-mode 1280 720'
sync_bridge restored-mode
"$UMBRIEL" settle

# Cancellation does not persist a denial and cannot leave late approval behind.
request org.test.Cancelled
physical 'tap 1'
bound "$SESSION" 1
expect_count "grant session=$SESSION " 0
send close
request org.test.Cancelled
physical 'tap 28'
bound "$SESSION" 0
send close
request org.test.ClientCancel
send cancel
sync_bridge client-cancel
bound "$SESSION" 1
review_status
[[ $VISIBLE == false ]]
physical 'tap 28'
sync_bridge late-enter
expect_count "grant session=$SESSION " 0
send close
request org.test.Destroyed
send close
sync_bridge destroyed
review_status
[[ $VISIBLE == false ]]
physical 'tap 28'
sync_bridge destroyed-enter
expect_count "grant session=$SESSION " 0

# Existing modals and another pending review fail contention without changing its owner.
"$UMBRIEL" msg session-quit > /dev/null
request org.test.Busy
bound "$SESSION" 2
physical status
await_events "$RUNTIME/compositor.log" 'status confirmation=1 ' 1
send close
physical 'tap 1'
request org.test.Busy
physical 'tap 28'
bound "$SESSION" 0
send close
request org.test.First
first=$SESSION
request org.test.Second
bound "$SESSION" 2
review_status
[[ $VISIBLE == true ]]
physical 'tap 28'
bound "$first" 0
expect_count "grant session=$SESSION " 0
send close "select $first" close

# Reload invalidates the candidates, while output loss fails pending review.
request org.test.Reload
"$UMBRIEL" msg config-reload > /dev/null
bound "$SESSION" 2
physical 'tap 28'
sync_bridge reload-enter
expect_count "grant session=$SESSION " 0
send close
request org.test.Small
physical 'output-mode 240 180'
sync_bridge too-small
"$UMBRIEL" settle
bound "$SESSION" 2
send close
request org.test.Small
bound "$SESSION" 2
review_status
[[ $VISIBLE == false ]]
send close
physical 'output-mode 1280 720'
sync_bridge usable-again
"$UMBRIEL" settle

# Lock preparation cancels pending consent, and locked sessions cannot prompt.
request org.test.Lock
mkfifo "$RUNTIME/lock"
exec {lock_fd}<> "$RUNTIME/lock"
"$BUILD/tests/lock-client" --defer-commit <&"$lock_fd" > "$RUNTIME/lock.log" 2>&1 &
PIDS+=("$!")
await_events "$RUNTIME/lock.log" configured 1
bound "$SESSION" 2
send close
printf 'commit\n' >&"$lock_fd"
await_events "$RUNTIME/lock.log" locked 1
request org.test.Locked
bound "$SESSION" 2
send close
printf 'unlock\n' >&"$lock_fd"
await_events "$RUNTIME/lock.log" unlocked 1

activations=$(events "$RUNTIME/shortcuts.log" activated)
for capability in 2 1; do
  capture new-session "application org.test.Capture$capability" "load-grant $RUNTIME/capture-$capability" "start $capability 2"
  await_events "$RUNTIME/capture.log" 'started .*status=0' "$((3 - capability))"
  zones=$(events "$RUNTIME/capture.log" 'zones request=')
  capture zones
  await_events "$RUNTIME/capture.log" 'zones request=' "$((zones + 1))"
  read -r width height < <(sed -n 's/^zone .*width=\([0-9]*\) height=\([0-9]*\).*/\1 \2/p' "$RUNTIME/capture.log" | tail -1)
  devices=$(events "$RUNTIME/capture.log" ei-device-ready)
  capture "barrier 1 $width 0 $width $((height - 1))" connect
  await_events "$RUNTIME/capture.log" ei-device-ready "$((devices + 1))"
  physical 'at 400 300' 'press 29' 'press 50'
  activations=$((activations + 1))
  await_events "$RUNTIME/shortcuts.log" activated "$activations"
  zones=$(events "$RUNTIME/capture.log" 'zones request=')
  capture enable zones
  await_events "$RUNTIME/capture.log" 'zones request=' "$((zones + 1))"
  physical "at $((width - 1)) $((height / 2))" 'move 20 0'
  await_events "$RUNTIME/capture.log" 'activated id=' "$((3 - capability))"
  await_events "$RUNTIME/shortcuts.log" deactivated "$activations"
  physical 'release 50' 'tap 50'
  "$BUILD/tests/pointer-client" 1280 720 keyboard-only mod control tap 50 mod none
  sync_bridge "capture-$capability"
  expect_count activated "$activations"
  expect_count deactivated "$activations"
  zones=$(events "$RUNTIME/capture.log" 'zones request=')
  capture disable zones
  await_events "$RUNTIME/capture.log" 'zones request=' "$((zones + 1))"
  markers=$(events "$RUNTIME/local.log" 'keyboard-key code=30 state=released')
  physical 'release 29' 'press 29' 'tap 50' 'release 29' 'tap 30'
  activations=$((activations + 1))
  await_events "$RUNTIME/shortcuts.log" activated "$activations"
  await_events "$RUNTIME/shortcuts.log" deactivated "$activations"
  await_events "$RUNTIME/local.log" 'keyboard-key code=30 state=released' "$((markers + 1))"
  presses=$(events "$RUNTIME/local.log" 'keyboard-key code=50 state=pressed')
  releases=$(events "$RUNTIME/local.log" 'keyboard-key code=50 state=released')
  if ((presses != releases)); then
    echo "capture left an unmatched shortcut release: $presses presses, $releases releases"
    exit 1
  fi
done
physical 'press 29' 'press 50'
activations=$((activations + 1))
await_events "$RUNTIME/shortcuts.log" activated "$activations"
physical quit
await_events "$RUNTIME/shortcuts.log" deactivated "$activations"
wait "$COMPOSITOR_PID"
SUCCESS=1
echo "physical shortcut approval, page review, cancellation, and capture handoff passed"
