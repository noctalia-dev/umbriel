#!/usr/bin/env bash
# hover_raises = false keeps hover to focus only: sweeping the cursor across a larger float underneath must not
# bury the window above it. Explicit clicks and navigation still raise, and a hover that skips the raise must leave
# the floating stack coherent for later restacks and focus navigation.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly SHOT="$UMBRIEL_RUNTIME_DIR/floating-hover-no-raise.png"
readonly APP_ID=hover-no-raise

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

wait_for_active() {
  local title=$1
  for _ in $(seq 80); do
    if windows | jq -e --arg title "$title" 'any(.[]; .title == $title and .active)' > /dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "expected $title to own keyboard focus: $(windows)"
  return 1
}

assert_pixel() {
  local x=$1 y=$2 expected=$3 context=$4 actual
  actual=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel "$x" "$y")
  if [[ $actual != "$expected" ]]; then
    echo "$context: expected RGB $expected at $x,$y, got $actual"
    return 1
  fi
}

capture() {
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$SHOT"
}

spawn() {
  local title=$1 color=$2 w=$3 h=$4
  env APP_ID="$APP_ID" FILL_COLOR="$color" RESIZE_FILL_COLOR="$color" \
    "$UMBRIEL_UNMAP_CLIENT" "$title" "$w" "$h" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  wait_for_window "$title"
}

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false

[input.focus]
follows_mouse = true
hover_raises = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
drag_opacity = 1.0
[appearance.shadow]
enabled = false

[[window_rule]]
match.app_id = "^hover-no-raise$"
default_workspace = 1
default_floating = true
default_focused = true

[[window_rule]]
match.app_id = "^hover-no-raise$"
match.title = "^hover-under$"
default_position = { x = 100, y = 100, anchor = "top_left" }

[[window_rule]]
match.app_id = "^hover-no-raise$"
match.title = "^hover-over$"
default_position = { x = 400, y = 300, anchor = "top_left" }

[[window_rule]]
match.app_id = "^hover-no-raise$"
match.title = "^hover-third$"
default_focused = false
default_position = { x = 750, y = 300, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

# hover-under (red) lies under hover-over (green) where they overlap at 500,400. 200,250 is hover-under alone;
# 650,520 is hover-over alone; hover-third (blue) maps away from both.
spawn hover-under 0xFFFF0000 500 400
spawn hover-over 0xFF00FF00 300 250
capture
wait_for_active hover-over
assert_pixel 500 400 "0 255 0" "hover-over did not map above hover-under"

# Hover focuses the covered float but must not raise it over the window it is sweeping across.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 200 250
capture
wait_for_active hover-under
assert_pixel 500 400 "0 255 0" "hovering hover-under raised it above hover-over"

# A click is explicit: it raises the float it lands on.
"$UMBRIEL_POINTER_CLIENT" 1280 720 click 272
capture
wait_for_active hover-under
assert_pixel 500 400 "255 0 0" "clicking hover-under did not raise it above hover-over"

# Mapping a window restacks the whole floating stack. A raise that left the stack out of step with the scene would
# replay a stale order here and bury hover-under again.
spawn hover-third 0xFF0000FF 250 250
capture
wait_for_active hover-under
assert_pixel 500 400 "255 0 0" "mapping hover-third replayed a stale floating order"

# Omitting hover_raises keeps today's behavior: hover raises.
sed -i 's/^hover_raises = false$/# hover_raises omitted: default true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 650 520
capture
wait_for_active hover-over
assert_pixel 500 400 "0 255 0" "the default hover_raises did not raise the hovered hover-over"

# Back to hover_raises = false. The no-raise path must keep the focus cycle ring intact, and hover focus must still
# promote the view so window-focus-last returns to the window focused before the hover.
sed -i 's/^# hover_raises omitted: default true$/hover_raises = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 200 250
capture
wait_for_active hover-under
assert_pixel 500 400 "0 255 0" "hovering hover-under raised it after the default-key check"

"$UMBRIEL" msg window-focus-last > /dev/null
wait_for_active hover-over

"$UMBRIEL_POINTER_CLIENT" 1280 720 move 200 250
wait_for_active hover-under
"$UMBRIEL" msg window-focus-next > /dev/null
wait_for_active hover-third
"$UMBRIEL" msg window-focus-previous > /dev/null
wait_for_active hover-over

echo "hover_raises = false keeps hover on focus alone while clicks, navigation, and the default hover-raises keep raising"
