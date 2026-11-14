#!/usr/bin/env bash
# Dwindle maximize: the window fills the area inside the gaps and the other tiles are hidden. Restoring returns the
# same tile. Focusing another tile, opening a new one, moving the tile away, or leaving dwindle ends the maximize.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/dwindle-maximize.png"

capture_config
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[colors]
backdrop = "#FFFFFFFF"
[layout]
mode = "dwindle"
gap = 20
[output.HEADLESS-1]
workspaces = 2
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

open() {
  FILL_COLOR=$2 FOLLOW_CONFIGURES=1 "$UMBRIEL_UNMAP_CLIENT" "$1" 400 300 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  await_window "$1"
}
box_of() { window_by_title "$1" | jq -c '[.x, .y, .w, .h]'; }
visible() { "$UMBRIEL" windows --json | jq -c '[.[] | select(.visible) | .title] | sort'; }
maximized() { "$UMBRIEL" windows --json | jq -c '[.[] | select(.maximized) | .title]'; }
focused() { "$UMBRIEL" windows --json | jq -r '.[] | select(.focused) | .title'; }
expect() {
  [[ $2 == "$3" ]] || { echo "$1: got $2, expected $3"; exit 1; }
}
toggle_maximize() {
  "$UMBRIEL" msg window-toggle-maximize > /dev/null
  "$UMBRIEL" settle
}

# The output is 1280x720: left takes the left half, top and bottom share the right half.
open left 0xFFFF0000
open top 0xFF00FF00
open bottom 0xFF0000FF
"$UMBRIEL" msg window-focus "$(window_by_title left | jq -r .id)" > /dev/null
"$UMBRIEL" settle
left_tile=$(box_of left)

toggle_maximize
expect "maximized box" "$(box_of left)" '[20,20,1240,680]'
expect "visible windows" "$(visible)" '["left"]'
grim "$FRAME"
assert_region_rgb "$FRAME" 20x20+1000+200 255 0 0 "left over the place of top"
assert_region_rgb "$FRAME" 20x20+1000+550 255 0 0 "left over the place of bottom"
assert_region_rgb "$FRAME" 4x4+2+360 255 255 255 "gap shows the backdrop"

toggle_maximize
expect "restored box" "$(box_of left)" "$left_tile"
expect "visible windows" "$(visible)" '["bottom","left","top"]'

toggle_maximize
"$UMBRIEL" msg window-focus-right > /dev/null
"$UMBRIEL" settle
expect "maximized windows" "$(maximized)" '[]'
expect "visible windows" "$(visible)" '["bottom","left","top"]'
[[ $(focused) != left ]] || { echo "focus stayed on left"; exit 1; }

"$UMBRIEL" msg window-focus "$(window_by_title left | jq -r .id)" > /dev/null
toggle_maximize
open new 0xFFFFFF00
"$UMBRIEL" settle
expect "maximized windows" "$(maximized)" '[]'
expect "visible windows" "$(visible)" '["bottom","left","new","top"]'

"$UMBRIEL" msg window-focus "$(window_by_title new | jq -r .id)" > /dev/null
toggle_maximize
"$UMBRIEL" msg window-move-to-workspace 2 > /dev/null
"$UMBRIEL" settle
expect "maximized windows" "$(maximized)" '[]'
expect "visible windows" "$(visible)" '["new"]'

"$UMBRIEL" msg workspace-switch 1 > /dev/null
"$UMBRIEL" msg window-focus "$(window_by_title left | jq -r .id)" > /dev/null
toggle_maximize
"$UMBRIEL" msg workspace-set-layout scrolling > /dev/null
"$UMBRIEL" settle
expect "maximized windows" "$(maximized)" '[]'
expect "visible windows" "$(visible)" '["bottom","left","top"]'

echo "dwindle maximize fills the gapped area, hides the other tiles, restores, and ends on focus, arrival, move, or layout change"
