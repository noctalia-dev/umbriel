#!/usr/bin/env bash
# harness: xwayland=true
# A physical keyboard authorizes EIS input. An already approved emulator cannot approve another application's prompt.
# X11 client teardown releases held input, and lock surfaces accept physical keys while emulated keys are discarded.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/eis_lib.sh"

readonly APPROVED=umbriel-xtest-approved
readonly DENIED=umbriel-xtest-denied
readonly WINDOW_LOG="$UMBRIEL_RUNTIME_DIR/eis-permission-window.log"
readonly LOCK_LOG="$UMBRIEL_RUNTIME_DIR/eis-permission-lock.log"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
LOG_MODIFIERS=1 "$NATIVE_CLIENT" eis-permission-window > "$WINDOW_LOG" 2>&1 &
eis_wait_window eis-permission-window

eis_start approved "$APPROVED"
eis_approve approved "$APPROVED"
eis_send approved 'tap 31'
await_events "$WINDOW_LOG" 'keyboard-key code=31 state=pressed' 1 "typing after physical consent"

eis_start denied "$DENIED"
eis_send denied 'tap 34'
eis_permission requested "$DENIED"
eis_send approved 'tap 28'
eis_sync approved fake-approval
sleep 0.2 # real time: the approved emulator's Enter must leave the other application's consent prompt open.
"$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only tap 1 > /dev/null
eis_permission denied "$DENIED"
if [[ $(events "$UMBRIEL_LOG" ".*input permission granted .*app='$DENIED'") -ne 0 ]]; then
  echo "emulated Enter granted another application's input permission: $(cat "$UMBRIEL_LOG")"
  exit 1
fi
eis_stop denied
eis_send approved 'tap 32'
await_events "$WINDOW_LOG" 'keyboard-key code=32 state=pressed' 1 "the original application after consent denial"
if [[ $(events "$WINDOW_LOG" 'keyboard-key code=34 state=pressed') -ne 0 ]]; then
  echo "the denied application's input reached the native window: $(cat "$WINDOW_LOG")"
  exit 1
fi

# Xwayland retains an EI connection for reuse when its X11 client exits. StopEmulating must release both input kinds
# immediately, rather than waiting for the EI connection's later timeout.
key_releases=$(events "$WINDOW_LOG" 'keyboard-key code=30 state=released')
key_presses=$(events "$WINDOW_LOG" 'keyboard-key code=30 state=pressed')
window=$(eis_window eis-permission-window)
pointer_x=$(jq -r '.x + .w / 2 | floor' <<< "$window")
pointer_y=$(jq -r '.y + .h / 2 | floor' <<< "$window")
eis_send approved "move $pointer_x $pointer_y" 'press 30' 'button 1 1'
await_events "$WINDOW_LOG" 'keyboard-key code=30 state=pressed' "$((key_presses + 1))" "the held emulated key"
await_events "$WINDOW_LOG" 'pointer-button code=272 state=pressed' 1 "the held emulated button"
eis_stop approved
await_events "$WINDOW_LOG" 'keyboard-key code=30 state=released' "$((key_releases + 1))" "key release on X11 client exit"
await_events "$WINDOW_LOG" 'pointer-button code=272 state=released' 1 "button release on X11 client exit"

eis_start resumed "$APPROVED"
eis_send resumed 'tap 33'
await_events "$WINDOW_LOG" 'keyboard-key code=33 state=pressed' 1 "the approved application after reconnect"

mkfifo "$UMBRIEL_RUNTIME_DIR/eis-lock.fifo"
exec {lock_fd}<> "$UMBRIEL_RUNTIME_DIR/eis-lock.fifo"
LOG_LOCK_INPUT=1 "$UMBRIEL_LOCK_CLIENT" <&"$lock_fd" > "$LOCK_LOG" 2>&1 &
lock_pid=$!
await_events "$LOCK_LOG" locked 1 "the real session lock"
await_events "$LOCK_LOG" keyboard-enter 1 "keyboard focus on the lock surface"

eis_send resumed 'tap 35'
eis_sync resumed locked-input
sleep 0.2 # real time: pending XTEST input must be processed while the lock is still held, without reaching its surface.
"$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only tap 31 > /dev/null
await_events "$LOCK_LOG" 'keyboard-key code=31 state=pressed' 1 "the physical keyboard on the lock surface"
if [[ $(events "$LOCK_LOG" 'keyboard-key code=35 state=pressed') -ne 0 ]]; then
  echo "emulated input reached the lock surface: $(cat "$LOCK_LOG")"
  exit 1
fi

printf 'unlock\n' >&"$lock_fd"
wait "$lock_pid"
exec {lock_fd}>&-
await_events "$LOCK_LOG" unlocked 1 "unlocking the session"
eis_send resumed 'tap 36'
await_events "$WINDOW_LOG" 'keyboard-key code=36 state=pressed' 1 "emulated input after unlock"
if [[ $(events "$WINDOW_LOG" 'keyboard-key code=35 state=pressed') -ne 0 ]]; then
  echo "XTEST input typed while locked was replayed after unlock: $(cat "$WINDOW_LOG")"
  exit 1
fi
if [[ $(events "$UMBRIEL_LOG" ".*input permission requested .*app='$APPROVED'") -ne 1 ]]; then
  echo "approved application lost session consent: $(cat "$UMBRIEL_LOG")"
  exit 1
fi
eis_stop resumed

echo "EIS consent requires physical approval, teardown releases input, and locking drops emulated keys without replay"
