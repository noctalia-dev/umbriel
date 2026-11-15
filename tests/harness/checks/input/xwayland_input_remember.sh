#!/usr/bin/env bash
# harness: xwayland=true
# R on the desktop control prompt remembers an X11 application in the state file. Revoking it prompts again.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/eis_lib.sh"

readonly REMEMBERED=umbriel-xtest-remembered
readonly WINDOW_LOG="$UMBRIEL_RUNTIME_DIR/eis-remember-window.log"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
"$NATIVE_CLIENT" eis-remember-window > "$WINDOW_LOG" 2>&1 &
eis_wait_window eis-remember-window

eis_start first "$REMEMBERED"
eis_send first 'tap 31'
eis_permission requested "$REMEMBERED"
"$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only tap 19 > /dev/null
eis_permission granted "$REMEMBERED"
await_events "$UMBRIEL_LOG" ".*input permission remembered app='$REMEMBERED'" 1 "remembering $REMEMBERED"
await_events "$WINDOW_LOG" 'keyboard-key code=31 state=pressed' 1 "typing after R"
eis_stop first

listed=$("$UMBRIEL" x11-input-permissions --json)
if [[ $(jq -c . <<< "$listed") != "[\"$REMEMBERED\"]" ]]; then
  echo "x11-input-permissions did not list the remembered application: $listed"
  exit 1
fi

if [[ $("$UMBRIEL" x11-input-permissions) != "$REMEMBERED" ]]; then
  echo "x11-input-permissions did not print the remembered application: $("$UMBRIEL" x11-input-permissions)"
  exit 1
fi

"$UMBRIEL" x11-input-revoke "$REMEMBERED" > /dev/null
if [[ $("$UMBRIEL" x11-input-permissions --json | jq length) -ne 0 ]]; then
  echo "x11-input-revoke left the application remembered: $("$UMBRIEL" x11-input-permissions --json)"
  exit 1
fi

eis_start second "$REMEMBERED"
eis_send second 'tap 32'
eis_permission requested "$REMEMBERED" 2
"$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only tap 1 > /dev/null
eis_permission denied "$REMEMBERED"
eis_stop second

# Revoking also forgets a denial, so the next request prompts again instead of being refused for the session.
"$UMBRIEL" x11-input-revoke "$REMEMBERED" > /dev/null
eis_start third "$REMEMBERED"
eis_send third 'tap 33'
eis_permission requested "$REMEMBERED" 3
"$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only tap 28 > /dev/null
eis_permission granted "$REMEMBERED" 2
await_events "$WINDOW_LOG" 'keyboard-key code=33 state=pressed' 1 "typing after a revoked denial"
eis_stop third
if [[ $(events "$WINDOW_LOG" 'keyboard-key code=32 state=pressed') -ne 0 ]]; then
  echo "input from the denied connection reached the native window: $(cat "$WINDOW_LOG")"
  exit 1
fi
if [[ $("$UMBRIEL" x11-input-permissions --json | jq length) -ne 0 ]]; then
  echo "Enter remembered the application: $("$UMBRIEL" x11-input-permissions --json)"
  exit 1
fi

echo "R remembers X11 input permission; x11-input-revoke forgets it, disconnects it, and clears a denial"
