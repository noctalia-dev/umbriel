#!/usr/bin/env bash
# harness: xwayland=true
# An application named in input.x11_input_allow emulates input without a consent prompt; others still prompt.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/eis_lib.sh"

readonly LISTED=umbriel-xtest-listed
readonly UNLISTED=umbriel-xtest-unlisted
readonly WINDOW_LOG="$UMBRIEL_RUNTIME_DIR/eis-allow-window.log"

cat >> "$UMBRIEL_CONFIG" <<CONFIG

[animation]
enabled = false

[input]
x11_input_allow = ["$LISTED"]
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
"$NATIVE_CLIENT" eis-allow-window > "$WINDOW_LOG" 2>&1 &
eis_wait_window eis-allow-window

eis_start listed "$LISTED"
eis_send listed 'tap 31'
await_events "$WINDOW_LOG" 'keyboard-key code=31 state=pressed' 1 "typing from the allowed application"
if [[ $(events "$UMBRIEL_LOG" ".*input permission requested .*app='$LISTED'") -ne 0 ]]; then
  echo "the allowed application was prompted: $(cat "$UMBRIEL_LOG")"
  exit 1
fi
eis_stop listed

eis_start unlisted "$UNLISTED"
eis_send unlisted 'tap 32'
eis_permission requested "$UNLISTED"
"$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only tap 1 > /dev/null
eis_permission denied "$UNLISTED"
eis_stop unlisted
if [[ $(events "$WINDOW_LOG" 'keyboard-key code=32 state=pressed') -ne 0 ]]; then
  echo "the unlisted application's input reached the native window: $(cat "$WINDOW_LOG")"
  exit 1
fi

echo "input.x11_input_allow skips the consent prompt only for listed applications"
