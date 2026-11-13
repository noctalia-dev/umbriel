#!/usr/bin/env bash
# hover_scrolls = false: hovering the visible part of a partly hidden scrolling column focuses it in place. No tiled
# window moves or resizes, even with hover_raises and a follows_mouse_max_scroll the reveal would exceed. A keybind
# afterwards still reveals, and on_overflow judges it from the hovered column.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly OUTPUT_W=1280

field() { window_by_title "$1" | jq -r ".$2"; }
geometry() { "$UMBRIEL" windows --json | jq -c '[.[] | select(.floating | not) | [.title, .x, .y, .w, .h]] | sort'; }

assert_active() {
  local actual
  actual=$("$UMBRIEL" windows --json | jq -r '[.[] | select(.active) | .title] | join(",")')
  [[ $actual == "$1" ]] || { echo "$2: expected $1 active, got '$actual': $(geometry)"; exit 1; }
}

assert_still() {
  local now
  now=$(geometry)
  [[ $now == "$1" ]] || { echo "$2: tiled geometry changed from $1 to $now"; exit 1; }
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "scrolling"

[layout.scrolling]
default_extent_fraction = 0.6
center_focused = "never"

[animation]
enabled = false

[input.focus]
follows_mouse = "sloppy"
follows_mouse_max_scroll = 0.0
hover_raises = true
hover_scrolls = false

[[window_rule]]
match.title = "^still-float$"
default_floating = true
default_floating_size_px = { width = 200, height = 150 }
default_position = { x = 1000, y = 20, anchor = "top_left" }
default_focused = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

for title in still-a still-b still-c; do
  # Follow configured tile sizes so the reported geometry is the tile itself.
  FILL_COLOR=0xFF808080 RESIZE_FILL_COLOR=0xFF808080 "$UMBRIEL_UNMAP_CLIENT" "$title" 400 300 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  await_window "$title"
done
"$UMBRIEL" settle > /dev/null
assert_active still-c "last mapped window"
# Map under never so C rests flush right instead of centered; on_overflow then applies to the moves below.
sed -i 's/center_focused = "never"/center_focused = "on_overflow"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null

before=$(geometry)
b_x=$(field still-b x)
b_right=$((b_x + $(field still-b w)))
c_x=$(field still-c x)
c_right=$((c_x + $(field still-c w)))
# Premise: B is cut by the left edge, and C is fully visible but not centered.
((b_x < 0 && b_right > 40 && c_x > 40 && c_right <= OUTPUT_W && c_x + c_right > OUTPUT_W + 8)) || { echo "still-b is not partly visible: $(geometry)"; exit 1; }

pointer_hold "$OUTPUT_W" 720 move $((c_x + 40)) 400 -- \
  move 20 400 mark hovered-b hold \
  move 1100 80 mark hovered-float hold \
  move 20 400 mark back-to-b hold
assert_active still-c "pointer resting on C"

pointer_step hovered-b
"$UMBRIEL" settle > /dev/null
assert_active still-b "hovering the visible part of B"
assert_still "$before" "hover focus on a partly visible column"

"$UMBRIEL_UNMAP_CLIENT" still-float 200 150 > "$UMBRIEL_RUNTIME_DIR/still-float.log" 2>&1 &
await_window still-float
"$UMBRIEL" settle > /dev/null
assert_active still-b "floating window mapped without focus"
pointer_step hovered-float
"$UMBRIEL" settle > /dev/null
assert_active still-float "hovering a floating window"
assert_still "$before" "hover focus on a floating window"

pointer_step back-to-b
"$UMBRIEL" settle > /dev/null
assert_active still-b "hovering B again"
assert_still "$before" "hover focus back onto B"
pointer_release

# Focus came to C from the hovered B, and the B/C pair cannot share the viewport, so on_overflow centers C. Had the hover
# not recorded B as the focused column, the move would be judged as coming from C itself and leave C where it rests.
"$UMBRIEL" msg window-focus-right > /dev/null
"$UMBRIEL" settle > /dev/null
assert_active still-c "keybind focus"
c_x=$(field still-c x)
c_center=$((c_x + $(field still-c w) / 2))
((c_center >= OUTPUT_W / 2 - 2 && c_center <= OUTPUT_W / 2 + 2)) || {
  echo "on_overflow keybind focus did not center C: $(geometry)"
  exit 1
}

# A keybind still reveals a hidden column.
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL" settle > /dev/null
assert_active still-a "keybind focus to the first column"
a_x=$(field still-a x)
((a_x >= 0 && a_x + $(field still-a w) <= OUTPUT_W)) || { echo "keybind did not reveal A: $(geometry)"; exit 1; }

echo "hover focused partly visible and floating windows without moving tiles; keybinds still revealed and centered"
