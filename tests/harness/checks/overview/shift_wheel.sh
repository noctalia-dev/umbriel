#!/usr/bin/env bash
# Wheel navigation uses physical axes and discrete column/workspace targets, with independent factors.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[overview]
scroll_factor_horizontal = 0.5
scroll_factor_vertical = 2.0

[layout.scrolling]
default_extent_fraction = 0.5

[output."HEADLESS-1"]
workspaces = 3
EOF
"$UMBRIEL" msg config-reload > /dev/null

for index in 1 2 3; do
  env FILL_COLOR="$((index == 2 ? 0xFF00FF00 : 0xFF0000FF))" \
    RESIZE_FILL_COLOR="$((index == 2 ? 0xFF00FF00 : 0xFF0000FF))" \
    "$UMBRIEL_UNMAP_CLIENT" "shift-wheel-$index" 1200 700 > /dev/null 2>&1 &
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq length) == "$index" ]] && break
    sleep 0.05
  done
done
[[ $("$UMBRIEL" windows --json | jq length) == 3 ]]
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL" msg overview-open > /dev/null

first_x() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "shift-wheel-1") | .x'
}
selected_title() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.focused) | .title'
}
active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .index'
}
before=$(first_x)
# Keep one virtual keyboard/pointer alive for the two half-factor notches.
pointer_hold 1280 720 move 640 360 mod shift notch 1 -- notch 1 mod none
[[ $(first_x) == "$before" ]] || { echo 'half-factor wheel moved on its first notch'; exit 1; }
[[ $(selected_title) == shift-wheel-1 ]] || { echo "half-factor first notch: $(selected_title)"; exit 1; }
pointer_release
for _ in $(seq 60); do
  [[ $(selected_title) == shift-wheel-2 ]] && break
  sleep 0.05
done
[[ $(selected_title) == shift-wheel-2 ]] || { echo 'Shift-wheel did not select the next column'; exit 1; }
[[ $(active_workspace) == 1 ]] || { echo 'horizontal wheel moved vertically between workspaces'; exit 1; }
(( $(first_x) < before )) || { echo 'Shift-wheel did not reveal the selected column'; exit 1; }
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod shift notch -1 notch -1 mod none
[[ $(selected_title) == shift-wheel-1 ]] || { echo 'reverse Shift-wheel did not select the previous column'; exit 1; }

# A partial notch belongs only to the current overview interaction.
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod shift notch 1 mod none
[[ $(selected_title) == shift-wheel-1 ]] || { echo 'half-factor wheel moved before overview close'; exit 1; }
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod shift notch 1 mod none
[[ $(selected_title) == shift-wheel-1 ]] || { echo 'partial wheel accumulation survived overview close'; exit 1; }
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod shift notch 1 mod none
[[ $(selected_title) == shift-wheel-2 ]] || { echo 'fresh half-factor notches did not select the next column'; exit 1; }

"$UMBRIEL" msg workspace-switch 1 > /dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 notch 1
for _ in $(seq 60); do
  [[ $(active_workspace) == 3 ]] && break
  sleep 0.05
done
[[ $(active_workspace) == 3 ]] || { echo 'double vertical factor did not advance two workspaces'; exit 1; }

# The same horizontal input follows the filmstrip when the workspace axis changes.
printf '\nworkspace_axis = "horizontal"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg workspace-switch 1 > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
# In this arrangement the vertical wheel steps columns, not workspaces. Factor 2 skips to column 3.
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 notch 1
[[ $(selected_title) == shift-wheel-3 ]] || { echo 'vertical wheel did not step vertical columns'; exit 1; }
[[ $(active_workspace) == 1 ]] || { echo 'vertical wheel switched horizontal workspaces'; exit 1; }
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod shift notch 1 notch 1 mod none
[[ $(active_workspace) == 2 ]] || { echo 'Shift-wheel did not follow horizontal workspaces'; exit 1; }

# A user binding wins, even though the default horizontal factor would require two notches.
printf '\n[keybinds]\n"Shift+WheelDown" = "workspace-next"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod shift notch 1 mod none
[[ $(active_workspace) == 3 ]] || { echo 'Shift-wheel ignored the configured binding'; exit 1; }
echo 'wheel steps vertically, Shift-wheel steps horizontally, factors count notches, and bindings win'

# Unequal neighbors make a stale direction observable: the incoming pair fits while the opposite pair overflows.
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" msg workspace-switch 1 > /dev/null
cat > "$UMBRIEL_CONFIG" <<'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[layout]
gap = 0
[layout.scrolling]
center_focused = "on_overflow"
[overview]
scroll_factor_horizontal = 1.0
scroll_factor_vertical = 1.0
[output."HEADLESS-1"]
workspaces = 3
workspace_axis = "vertical"
EOF
"$UMBRIEL" msg config-reload > /dev/null

set_column_extent() {
  local id
  id=$("$UMBRIEL" windows --json | jq -r --arg title "shift-wheel-$1" '.[] | select(.title == $title) | .id')
  "$UMBRIEL" msg window-focus "$id" > /dev/null
  "$UMBRIEL" msg window-set-primary-extent "$2" > /dev/null
  "$UMBRIEL" settle
}

assert_fitting_wheel_pair() {
  local direction=$1 expected_x=$2 screenshot="$UMBRIEL_RUNTIME_DIR/wheel-pair-$1.png" box
  "$UMBRIEL" msg overview-open > /dev/null
  "$UMBRIEL" settle
  "$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 360 notch-horizontal "$direction"
  [[ $(selected_title) == shift-wheel-2 ]] || { echo 'wheel did not select the middle column'; exit 1; }
  "$UMBRIEL" msg overview-close > /dev/null
  "$UMBRIEL" settle
  grim "$screenshot"
  box=$("$UMBRIEL_PIXEL_PROBE" "$screenshot" bbox 'g > 0.9 && r < 0.1 && b < 0.1' '1280x1+0+360')
  [[ $box == "$expected_x 360 400 1" ]] || {
    echo "wheel $direction centered a fitting pair using the opposite neighbor: green bbox=$box"
    exit 1
  }
}

# Focus moves left before the rightward wheel, leaving the opposite direction in the layout's remembered pair.
set_column_extent 3 0.703125
set_column_extent 2 0.3125
set_column_extent 1 0.3125
assert_fitting_wheel_pair 1 0

# Mirror the widths and preceding focus direction for a leftward wheel.
set_column_extent 1 0.703125
set_column_extent 2 0.3125
set_column_extent 3 0.3125
assert_fitting_wheel_pair -1 480
echo 'wheel uses the incoming neighbor for on_overflow in both directions'
