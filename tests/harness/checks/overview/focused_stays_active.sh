#!/usr/bin/env bash
# harness: outputs=2
# The window the overview would act on stays active while it is open, as it does when focus moves there, and follows
# the pointer between outputs along with the focused card border. The overview still holds the keyboard: a key reaches
# that window only after it closes.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly LEFT_LOG="$UMBRIEL_RUNTIME_DIR/left.log"
readonly RIGHT_LOG="$UMBRIEL_RUNTIME_DIR/right.log"
readonly KEY_A=30

cat >> "$UMBRIEL_CONFIG" << 'EOF'

[output.HEADLESS-1]
position = [0, 0]

[output.HEADLESS-2]
position = [1280, 0]

[[window_rule]]
match.title = "^left$"
default_output = "HEADLESS-1"

[[window_rule]]
match.title = "^right$"
default_output = "HEADLESS-2"
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_windows() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') == "$1" ]] && return 0
    sleep 0.05
  done
  echo "expected $1 window(s): $("$UMBRIEL" windows --json)"
  return 1
}

move_pointer_to() {
  "$POINTER" 2560 720 move "$1" 360 > /dev/null
  "$UMBRIEL" settle
}

type_key() {
  "$POINTER" 2560 720 keyboard-only tap "$KEY_A" > /dev/null
}

expect_active() {
  local active
  active=$("$UMBRIEL" windows --json | jq -c '[.[] | select(.active) | .title]')
  if [[ $active != "$1" ]]; then
    echo "$2: expected $1 active, got: $active"
    exit 1
  fi
}

"$CLIENT" left > "$LEFT_LOG" 2>&1 &
wait_for_windows 1
"$CLIENT" right > "$RIGHT_LOG" 2>&1 &
wait_for_windows 2
"$UMBRIEL" settle
move_pointer_to 1920

"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
expect_active '["right"]' "overview just opened"

type_key
if grep -q "^key $KEY_A " "$RIGHT_LOG"; then
  echo "a key reached the active window while the overview was open"
  exit 1
fi

move_pointer_to 640
expect_active '["left"]' "pointer moved to the other output"
move_pointer_to 1920
expect_active '["right"]' "pointer moved back"
move_pointer_to 640

"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle
type_key
for _ in $(seq 40); do
  grep -q "^key $KEY_A " "$LEFT_LOG" && break
  sleep 0.05
done
if ! grep -q "^key $KEY_A " "$LEFT_LOG"; then
  echo "a key did not reach the active window after the overview closed: $(cat "$LEFT_LOG")"
  exit 1
fi

# An output with nothing to act on leaves no window active until the pointer comes back.
"$UMBRIEL" msg workspace-switch 2 --output HEADLESS-1 > /dev/null
move_pointer_to 1920
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
expect_active '["right"]' "overview opened beside an empty workspace"
move_pointer_to 640
expect_active '[]' "pointer moved to the empty workspace"
move_pointer_to 1920
expect_active '["right"]' "pointer moved back from the empty workspace"

echo "the active window followed the overview's target between outputs and got the keyboard back when it closed"
