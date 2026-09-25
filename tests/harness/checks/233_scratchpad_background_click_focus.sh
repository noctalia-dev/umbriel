#!/usr/bin/env bash
# Clicking the exposed workspace background is an explicit focus action and must
# steal keyboard focus back from a visible scratchpad. This is the counterpart
# to the workspace-switch retention check: refocusExplicit(Output*) always
# selects the workspace fallback, so an empty-background click reaches the
# workspace window instead of being retained on the scratchpad.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly SCRATCHPAD=background
readonly WS=scratchpad-background-workspace
readonly SCRATCH=scratchpad-background-foreground
# Far from both the workspace window (top-left) and the centered scratchpad.
readonly EMPTY_X=1200
readonly EMPTY_Y=650

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
scale = 0

[input.focus]
follows_mouse = false

[[scratchpad]]
name = "background"

[[window_rule]]
match.title = "^scratchpad-background-workspace$"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 50, y = 50, anchor = "top_left" }

[[window_rule]]
match.title = "^scratchpad-background-foreground$"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
default_position = { x = 430, y = 230, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }

field_of() {
  windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_count() {
  local expected=$1 count=
  for _ in $(seq 80); do
    count=$(windows | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected windows, got $count: $(windows)"
  return 1
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual': $(windows)"
  return 1
}

"$CLIENT" "$WS" > "$UMBRIEL_RUNTIME_DIR/$WS.log" 2>&1 &
wait_for_count 1
wait_for_field "$WS" active true

"$CLIENT" "$SCRATCH" 420 260 > "$UMBRIEL_RUNTIME_DIR/$SCRATCH.log" 2>&1 &
wait_for_count 2
scratch_id=$(field_of "$SCRATCH" id)
"$UMBRIEL" msg "window-focus:$scratch_id" > /dev/null
"$UMBRIEL" msg "window-move-to-scratchpad:$SCRATCHPAD" > /dev/null
wait_for_field "$SCRATCH" scratchpad "$SCRATCHPAD"
"$UMBRIEL" msg "scratchpad-toggle:$SCRATCHPAD" > /dev/null
wait_for_field "$SCRATCH" active true
"$UMBRIEL" msg "window-focus:$scratch_id" > /dev/null
wait_for_field "$SCRATCH" active true

# Precondition for the explicit path: the scratchpad owns focus while the
# workspace window sits behind it.
if [[ $(field_of "$WS" active) != false ]]; then
  echo "scratchpad did not take focus from the workspace window: $(windows)"
  exit 1
fi

# Clicking empty background hits no view, so Cursor routes it to
# refocusExplicit() which must fall back to the workspace window.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$EMPTY_X" "$EMPTY_Y" click "$BTN_LEFT"
wait_for_field "$WS" active true
wait_for_field "$SCRATCH" active false

echo "background click moves focus from the visible scratchpad to the workspace window"
