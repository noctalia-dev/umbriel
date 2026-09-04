#!/usr/bin/env bash
# session-quit asks for confirmation; session-quit:skip-confirmation does not. The confirmation path runs against the
# instance the harness booted for this check, which it is free to terminate: check.sh notices an already-exited
# compositor and judges it by its exit status. So the first session-quit must open the dialog and leave the instance
# answering IPC, and the second must take it down cleanly. Proving the bypass form then needs a live compositor again,
# so this check boots exactly one private instance for it, with the same containment as check.sh.
set -euo pipefail

# The harness instance is not this check's child, so there is no pid to signal
# and liveness can only be observed over IPC.
answers_ipc() { "$UMBRIEL" windows > /dev/null 2>&1; }

# The first invocation opens the dialog and must NOT terminate the compositor.
if ! "$UMBRIEL" msg session-quit > /dev/null 2>&1; then
  echo "msg session-quit was rejected"
  exit 1
fi
sleep 0.5
if ! answers_ipc; then
  echo "compositor stopped answering IPC after a single session-quit"
  exit 1
fi

# A second invocation confirms and shuts down. The IPC reply may be lost as the
# display terminates, so tolerate a failed msg; the harness asserts that this
# instance exited with status 0.
"$UMBRIEL" msg session-quit > /dev/null 2>&1 || true
quit=false
for _ in $(seq 40); do
  if ! answers_ipc; then
    quit=true
    break
  fi
  sleep 0.25
done
if [[ $quit != true ]]; then
  echo "compositor kept answering IPC after a second session-quit"
  exit 1
fi

# sockaddr_un caps paths at 108 bytes and the compositor appends
# "/umbriel-wayland-0.sock" (23) to XDG_RUNTIME_DIR, so keep the root short.
RUNTIME_DIR=$(mktemp -d /tmp/umq.XXXXXXXX)
SERVER_PID=

cleanup() {
  if [[ -n $SERVER_PID ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill -KILL "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$RUNTIME_DIR"
}
trap cleanup EXIT

LOG=$RUNTIME_DIR/compositor.log
CONFIG=$RUNTIME_DIR/config.toml
SOCKET=$RUNTIME_DIR/umbriel-wayland-0.sock
cat > "$CONFIG" << 'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
EOF

env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
  XDG_RUNTIME_DIR="$RUNTIME_DIR" \
  WLR_BACKENDS=headless \
  WLR_LIBINPUT_NO_DEVICES=1 \
  WLR_HEADLESS_OUTPUTS=1 \
  "$UMBRIEL" -c "$CONFIG" > "$LOG" 2>&1 &
SERVER_PID=$!
export UMBRIEL_SOCKET=$SOCKET

for _ in $(seq 40); do
  [[ -S $SOCKET ]] && break
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "the private compositor died during boot"
    sed 's/^/  | /' "$LOG"
    exit 1
  fi
  sleep 0.25
done
if [[ ! -S $SOCKET ]]; then
  echo "the private compositor never exposed its IPC socket"
  sed 's/^/  | /' "$LOG"
  exit 1
fi

# The bypass form terminates in a single call.
"$UMBRIEL" msg session-quit:skip-confirmation > /dev/null 2>&1 || true
for _ in $(seq 40); do
  kill -0 "$SERVER_PID" 2>/dev/null || break
  sleep 0.25
done
if kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "compositor survived session-quit:skip-confirmation"
  exit 1
fi
status=0
wait "$SERVER_PID" || status=$?
SERVER_PID=
if [[ $status -ne 0 ]]; then
  echo "the private compositor exited with status $status on bypass, expected 0"
  exit 1
fi

echo "session-quit confirms; skip-confirmation quits immediately"
