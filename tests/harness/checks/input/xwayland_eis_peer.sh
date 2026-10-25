#!/usr/bin/env bash
# harness: xwayland=true
# A process that knows the private socket path and claims an approved X11 application name still cannot connect as
# Xwayland. The real libei handshake must reject it before an input seat or a new consent prompt is exposed.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/eis_lib.sh"

readonly APP=umbriel-xtest-peer
readonly WINDOW_LOG="$UMBRIEL_RUNTIME_DIR/eis-peer-window.log"
readonly PEER_LOG="$UMBRIEL_RUNTIME_DIR/eis-peer-client.log"

"$NATIVE_CLIENT" eis-peer-window > "$WINDOW_LOG" 2>&1 &
eis_wait_window eis-peer-window
eis_start authorized "$APP"
eis_approve authorized "$APP"
eis_send authorized 'tap 31'
await_events "$WINDOW_LOG" 'keyboard-key code=31 state=pressed' 1 "the approved Xwayland connection"

socket=$(eis_socket)
if ! "$EI_PEER_CLIENT" "$socket" "$APP" > "$PEER_LOG" 2>&1; then
  cat "$PEER_LOG"
  exit 1
fi
await_events "$PEER_LOG" rejected 1 "the non-Xwayland peer claiming an approved name"
if [[ $(events "$UMBRIEL_LOG" ".*input permission requested .*app='$APP'") -ne 1 ]]; then
  echo "the untrusted EI peer reached the consent UI: $(cat "$UMBRIEL_LOG")"
  exit 1
fi

# Rejection must leave the authentic Xwayland connection usable.
eis_send authorized 'tap 32'
await_events "$WINDOW_LOG" 'keyboard-key code=32 state=pressed' 1 "the authentic client after peer rejection"
eis_stop authorized

echo "private EIS socket rejects non-Xwayland peers even when they claim an approved application name"
