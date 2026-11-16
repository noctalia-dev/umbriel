#!/usr/bin/env bash
# Backend-boundary input in a private compositor verifies capture handoff without touching the host session.
set -euo pipefail
umask 077
readonly BUILD=$(realpath "${1:?usage: global_shortcuts.sh BUILD_DIRECTORY}")
readonly RUNTIME=$(mktemp -d /tmp/umbriel-shortcuts-input.XXXXXXXX)
readonly CLIENT=$BUILD/tests/global-shortcuts-client
readonly COMPOSITOR=$BUILD/tests/input-capture-compositor
readonly REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PIDS=()
SUCCESS=0
cleanup() {
  local pid
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  if ((SUCCESS)); then
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

activations=0
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
  physical 'release 29' 'press 29' 'tap 50' 'release 29' 'tap 30'
  activations=$((activations + 1))
  await_events "$RUNTIME/shortcuts.log" activated "$activations"
  await_events "$RUNTIME/shortcuts.log" deactivated "$activations"
  await_events "$RUNTIME/local.log" 'keyboard-key code=30 state=released' "$((3 - capability))"
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
echo "keyboard and pointer capture retire held shortcuts and require fresh input after handoff"
