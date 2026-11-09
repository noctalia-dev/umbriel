#!/usr/bin/env bash
# A scratchpad shown over a fullscreen window draws above it, backdrop included, instead of taking focus while hidden
# beneath it. Otherwise it stays below panels, and pinned windows stay above it either way.
set -euo pipefail

readonly SHOT="$UMBRIEL_RUNTIME_DIR/scratchpad-above-fullscreen.png"

windows() { "$UMBRIEL" windows --json; }

wait_for_window() {
  local title=$1
  for _ in $(seq 80); do
    if windows | jq -e --arg title "$title" 'any(.[]; .title == $title)' > /dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "window $title did not map: $(windows)"
  return 1
}

capture() {
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$SHOT"
}

pixel() { "$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel "$1" "$2"; }

assert_pixel() {
  local x=$1 y=$2 expected=$3 context=$4 actual
  actual=$(pixel "$x" "$y")
  if [[ $actual != "$expected" ]]; then
    echo "$context: expected RGB $expected at $x,$y, got $actual"
    return 1
  fi
}

assert_active() {
  local title=$1
  if ! windows | jq -e --arg title "$title" 'any(.[]; .title == $title and .active)' > /dev/null; then
    echo "expected $title to own keyboard focus: $(windows)"
    return 1
  fi
}

cat >> "$UMBRIEL_CONFIG" << 'EOF'

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false

[animation]
enabled = false

[animation.scratchpad]
dim = 0.6
blur = false

[[scratchpad]]
name = "test"

[[window_rule]]
match.title = "^pad$"
default_scratchpad = "test"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 440, y = 210, anchor = "top_left" }

[[window_rule]]
match.title = "^fullscreen$"
default_fullscreen = true

[[window_rule]]
match.title = "^pinned$"
default_floating = true
default_pinned = true
default_floating_size_px = { width = 200, height = 200 }
default_position = { x = 540, y = 260, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

# A 40px top panel, painted 0x202020.
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 keyboard=none > "$UMBRIEL_RUNTIME_DIR/panel.log" 2>&1 &
FILL_COLOR=0xFFFF0000 "$UMBRIEL_UNMAP_CLIENT" pad 400 300 > "$UMBRIEL_RUNTIME_DIR/pad.log" 2>&1 &
wait_for_window pad

# Nothing fullscreen: the pad shows below the panel, which stays undimmed.
"$UMBRIEL" msg scratchpad-toggle test > /dev/null
capture
assert_pixel 640 360 "255 0 0" "the shown pad is not on screen"
assert_pixel 640 20 "32 32 32" "the pad's backdrop covered the panel without a fullscreen window"
"$UMBRIEL" msg scratchpad-toggle test > /dev/null

FILL_COLOR=0xFF0000FF RESIZE_FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" fullscreen 1280 720 \
  > "$UMBRIEL_RUNTIME_DIR/fullscreen.log" 2>&1 &
wait_for_window fullscreen
capture
assert_pixel 640 360 "0 0 255" "the fullscreen window does not cover the output"

# Over fullscreen, both the pad and its backdrop draw on top, and the visible pad has focus.
"$UMBRIEL" msg scratchpad-toggle test > /dev/null
capture
assert_active pad
assert_pixel 640 360 "255 0 0" "the pad stayed beneath the fullscreen window"
read -r _ _ blue < <(pixel 100 600)
if (( blue > 150 )); then
  echo "the pad's backdrop stayed beneath the fullscreen window: blue=$blue"
  exit 1
fi

# Hiding the pad shows the fullscreen window again.
"$UMBRIEL" msg scratchpad-toggle test > /dev/null
capture
assert_active fullscreen
assert_pixel 640 360 "0 0 255" "the hidden pad left content over the fullscreen window"

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
"$UMBRIEL" msg window-close > /dev/null
for _ in $(seq 80); do
  windows | jq -e 'all(.[]; .title != "fullscreen")' > /dev/null && break
  sleep 0.025
done

# A pinned window stays above the pad.
FILL_COLOR=0xFF00FF00 "$UMBRIEL_UNMAP_CLIENT" pinned 200 200 > "$UMBRIEL_RUNTIME_DIR/pinned.log" 2>&1 &
wait_for_window pinned
capture
assert_pixel 640 360 "0 255 0" "the pinned window is not on screen"
"$UMBRIEL" msg scratchpad-toggle test > /dev/null
capture
assert_active pad
assert_pixel 640 360 "0 255 0" "the pad covered the pinned window"
assert_pixel 460 520 "255 0 0" "the pad is not on screen around the pinned window"

echo "a scratchpad draws above a fullscreen window on its output, below panels otherwise, and below pinned windows"
