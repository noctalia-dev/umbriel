#!/usr/bin/env bash
# The cheatsheet renders without taking the compositor down, and a pointer press dismisses it. Its render path walks every configured keybind, builds a label per bind from the action spec and the bind's payload, then lays the result out with pango. None of that is observable over IPC, so this cannot assert what is drawn. What it can assert is that building and tearing down the overlay repeatedly leaves a live, responsive compositor, which is what a bad label lookup or a null payload would break. Visibility is not observable either, so the dismissal is observed through the press it consumes: while the overlay is up, a click over an unfocused window must not move focus (the press is swallowed), and the next click must (the overlay is gone). One assertion alone would pass with the dismissal missing or with the swallow missing; the pair only passes when both happen.
set -euo pipefail

readonly BTN_LEFT=272 # evdev BTN_LEFT
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

printf '\n[layout.scrolling]\ndefault_extent_fraction = 0.5\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

alive() {
  if ! "$UMBRIEL" windows --json > /dev/null 2>&1; then
    echo "compositor stopped answering after $1"
    return 1
  fi
}

"$UMBRIEL" msg cheatsheet-open > /dev/null
alive "cheatsheet-open"

"$UMBRIEL" msg cheatsheet-close > /dev/null
alive "cheatsheet-close"

# Toggling repeatedly exercises the destroy-and-rebuild path, which is how the
# overlay is re-rendered on every relayout.
for _ in 1 2 3; do
  "$UMBRIEL" msg cheatsheet-toggle > /dev/null
done
alive "cheatsheet-toggle"

"$UMBRIEL" msg cheatsheet-close > /dev/null
alive "final close"

echo "open, close, and repeated toggle all survive"

if [[ ! -x $POINTER ]]; then
  echo "pointer client not built at $POINTER"
  exit 1
fi

spawn_client() {
  foot sh -c 'sleep 120' > /dev/null 2>&1 &
}

# The output size is the pointer client's coordinate space, not an environment
# concern, so this wrapper stays.
pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

wait_for_count() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s)"
  return 1
}

# x of the focused window, or "none".
focused_x() { "$UMBRIEL" windows --json | jq -r '[.[] | select(.focused) | .x] | if length == 1 then .[0] else "none" end'; }

wait_for_focus_at() {
  local want=$1
  for _ in $(seq 40); do
    [[ $(focused_x) == "$want" ]] && return 0
    sleep 0.25
  done
  echo "expected the window at x=$want to be focused, focus is at $(focused_x)"
  echo "  windows: $("$UMBRIEL" windows --json | jq -c '[.[] | {x, focused}]')"
  return 1
}

spawn_client
wait_for_count 1
spawn_client
wait_for_count 2

# Two 624-wide columns at x=10 and x=646, both 700 tall from y=10.
readonly LEFT_X=322  # 10 + 624/2
readonly RIGHT_X=958 # 646 + 624/2
readonly MID_Y=360

pointer move "$LEFT_X" "$MID_Y" click "$BTN_LEFT"
wait_for_focus_at 10

"$UMBRIEL" msg cheatsheet-open > /dev/null

# The dismissing press is consumed, so focus stays on the left window even
# though the cursor is over the right one.
pointer move "$RIGHT_X" "$MID_Y" click "$BTN_LEFT"
"$UMBRIEL" settle
if [[ $(focused_x) != "10" ]]; then
  echo "the dismissing click reached the window under the cursor: focus moved to $(focused_x)"
  exit 1
fi
alive "cheatsheet dismiss click"

# The overlay is gone now, so the same click focuses normally.
pointer click "$BTN_LEFT"
wait_for_focus_at 646

echo "a pointer press dismisses the overlay and is not delivered to the window under it"
