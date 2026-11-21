#!/usr/bin/env bash
# A compact overview keeps workspace identity while skipping empty fixed slots.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/hide-empty.png"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[output.HEADLESS-1]
workspaces = 5
[overview]
hide_empty_workspaces = true
workspace_wallpaper = false
background_blur = false
shortcuts = false
[colors]
backdrop = "#000000FF"
[colors.overview]
background_tint = "#00000000"
workspace_background = "#00FF00FF"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[animation]
duration_ms = 1
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFFFF0000 "$UMBRIEL_UNMAP_CLIENT" hide-red 1280 720 > "$UMBRIEL_RUNTIME_DIR/red.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/red.log" mapped 1
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
"$UMBRIEL" msg workspace-switch 3 > /dev/null
FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" hide-blue 1280 720 > "$UMBRIEL_RUNTIME_DIR/blue.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/blue.log" mapped 1
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
"$UMBRIEL" msg workspace-switch 1 > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
grim "$SHOT"
# Workspace 3 must appear immediately below workspace 1, where workspace 2
# would have appeared with the unfiltered fixed inventory.
read -r red green blue <<< "$("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel 640 620)"
if (( blue < 200 || red > 30 || green > 30 )); then
  echo "expected workspace 3's blue window beside workspace 1; pixel was $red $green $blue"
  exit 1
fi
# The visible row selects its real workspace, not its compacted row number.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 620 click 272
"$UMBRIEL" settle
active=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name')
[[ $active == 3 ]] || { echo "click selected workspace $active instead of 3"; exit 1; }

# Empty workspaces selected explicitly must be visible while active.
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" msg workspace-switch 2 > /dev/null
"$UMBRIEL" settle
active=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name')
[[ $active == 2 ]] || { echo "direct selection reached $active instead of 2"; exit 1; }
grim "$SHOT"
read -r red green blue <<< "$("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel 640 360)"
if (( green < 200 || red > 30 || blue > 30 )); then
  echo "active empty workspace did not have a preview: $red $green $blue"
  exit 1
fi
"$UMBRIEL" msg workspace-switch 1 > /dev/null
"$UMBRIEL" settle
# Move the active window to a previously hidden workspace while the overview
# is open. Its new row must appear at its correct compacted position.
"$UMBRIEL" msg window-move-to-workspace-silent 5 > /dev/null
"$UMBRIEL" settle
workspace=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "hide-red") | .workspace')
[[ $workspace == HEADLESS-1:5 ]] || { echo "window moved to $workspace instead of 5"; exit 1; }
"$UMBRIEL" msg workspace-switch 3 > /dev/null
"$UMBRIEL" settle
grim "$SHOT"
read -r red green blue <<< "$("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel 640 620)"
if (( red < 200 || green > 30 || blue > 30 )); then
  echo "workspace 5 did not appear after its window moved: $red $green $blue"
  exit 1
fi

"$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 360 notch 1
active=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name')
[[ $active == 5 ]] || { echo "wheel selected workspace $active instead of 5"; exit 1; }
"$UMBRIEL_POINTER_CLIENT" 1280 720 notch -1
active=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name')
[[ $active == 3 ]] || { echo "reverse wheel selected workspace $active instead of 3"; exit 1; }
"$UMBRIEL" settle
"$UMBRIEL_POINTER_CLIENT" 1280 720 axis vertical 210 pause 180 axis-stop vertical
"$UMBRIEL" settle
active=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name')
[[ $active == 5 ]] || { echo "touchpad selected workspace $active instead of 5"; exit 1; }
"$UMBRIEL_POINTER_CLIENT" 1280 720 axis vertical -210 pause 180 axis-stop vertical
"$UMBRIEL" settle
active=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name')
[[ $active == 3 ]] || { echo "reverse touchpad selected workspace $active instead of 3"; exit 1; }

# Dropping a card onto a compacted row must move it to that row's actual
# workspace. Once workspace 5 becomes empty, its preview must disappear.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 620 press 272 move 640 500 move 640 360 release 272
"$UMBRIEL" settle
workspace=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "hide-red") | .workspace')
[[ $workspace == HEADLESS-1:3 ]] || { echo "drag moved the window to $workspace instead of 3"; exit 1; }
grim "$SHOT"
read -r red green blue <<< "$("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel 640 620)"
if (( red > 30 || green > 30 || blue > 30 )); then
  echo "empty workspace 5 still has a preview: $red $green $blue"
  exit 1
fi

# The same mapping applies when workspace previews run horizontally.
"$UMBRIEL" msg window-move-to-workspace-silent 5 > /dev/null
sed -i '/^workspaces = 5$/a workspace_axis = "horizontal"' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg workspace-switch 3 > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
grim "$SHOT"
read -r red green blue <<< "$("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel 1100 360)"
if (( red < 200 || green > 30 || blue > 30 )); then
  echo "horizontal compacted workspace 5 was not visible: $red $green $blue"
  exit 1
fi
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 360 notch-horizontal 1
active=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name')
[[ $active == 5 ]] || { echo "horizontal wheel selected workspace $active instead of 5"; exit 1; }

# Reconfiguring to a dynamic inventory may renumber workspaces. Navigation
# must continue to follow the visible windows rather than stale row indices.
sed -i 's/^workspaces = 5$/workspaces = "dynamic"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 360 notch-horizontal -1
"$UMBRIEL" settle
active_title=$("$UMBRIEL" windows --json | jq -r '.[] | select(.active) | .title')
[[ $active_title == hide-blue ]] || { echo "dynamic workspace navigation focused $active_title instead of hide-blue"; exit 1; }
echo 'empty previews are compacted while direct selection, drag, navigation, and inventory changes retain workspace identity'
