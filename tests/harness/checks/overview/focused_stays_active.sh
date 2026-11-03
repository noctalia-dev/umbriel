#!/usr/bin/env bash
# The focused window stays active while the overview is open, as it does when focus moves there. The overview still
# holds the keyboard: a key reaches the window only after it closes.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/focused.log"
readonly KEY_A=30

wait_for_windows() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') == "$1" ]] && return 0
    sleep 0.05
  done
  echo "expected $1 window(s): $("$UMBRIEL" windows --json)"
  return 1
}

type_key() {
  "$POINTER" 1280 720 keyboard-only tap "$KEY_A" > /dev/null
}

"$CLIENT" other > /dev/null 2>&1 &
wait_for_windows 1
"$CLIENT" focused > "$CLIENT_LOG" 2>&1 &
wait_for_windows 2
"$UMBRIEL" settle

"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

active=$("$UMBRIEL" windows --json | jq -c '[.[] | select(.active) | .title]')
if [[ $active != '["focused"]' ]]; then
  echo "expected only the focused window to stay active in the overview, got: $active"
  exit 1
fi

type_key
if grep -q "^key $KEY_A " "$CLIENT_LOG"; then
  echo "a key reached the focused window while the overview was open"
  exit 1
fi

"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle
type_key
for _ in $(seq 40); do
  grep -q "^key $KEY_A " "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q "^key $KEY_A " "$CLIENT_LOG"; then
  echo "a key did not reach the focused window after the overview closed: $(cat "$CLIENT_LOG")"
  exit 1
fi

echo "the focused window stayed active in the overview and got the keyboard back when it closed"
