#!/usr/bin/env bash
# Continuous touchpad navigation in the overview, on both workspace arrangements: travel across the workspace axis
# pans the strip in one preview, travel along it drags the filmstrip and selects only on release, the release keeps
# moving under the configured spring, losing the device cancels, and each physical axis has its own scroll factor.
# The headless backend has no touchpad and zwlr_virtual_pointer_v1 carries no gesture events, so this exercises the
# finger-scroll stream; three-finger swipes reach the same navigation with their own travel distances.
set -euo pipefail

active_workspace() { "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .index'; }
wait_workspace() {
  for _ in $(seq 60); do
    [[ $(active_workspace) == "$1" ]] && return 0
    sleep 0.05
  done
  echo "expected workspace $1: $("$UMBRIEL" workspaces --json)"
  return 1
}
frame() { grim "$UMBRIEL_RUNTIME_DIR/$1.png"; }
# Mean blue of a patch of the first preview: it drops as the filmstrip moves a card out from under it.
blue() {
  frame "$1"
  magick "$UMBRIEL_RUNTIME_DIR/$1.png" -crop 20x20+630+350 -format '%[fx:round(255*mean.b)]' info:
}
# Mean difference between two frames, as a fraction of full scale.
difference() {
  magick "$UMBRIEL_RUNTIME_DIR/$1.png" "$UMBRIEL_RUNTIME_DIR/$2.png" \
    -compose difference -composite -format '%[fx:mean]' info:
}
moving() { awk -v value="$(difference "$1" "$2")" 'BEGIN { exit !(value > 0.0005) }'; }
# $1 workspace axis, $2 vertical scroll factor, $3 horizontal scroll factor.
configure() {
  cat > "$UMBRIEL_CONFIG" <<EOF
[general]
xwayland = false
show_cheatsheet = false
autostart = []

[animation.overview]
# Slow enough that the motion after release is observable, which also pins workspace_curve to the spring path.
workspace_curve = "spring:1,120"

[layout.scrolling]
default_width_fraction = 0.5

[overview]
background_blur = false
shortcuts = false
scroll_factor_vertical = $2
scroll_factor_horizontal = $3

[output."HEADLESS-1"]
workspace_axis = "$1"
EOF
  "$UMBRIEL" msg config-reload > /dev/null
}

count=0
for title in touchpad-a touchpad-b touchpad-c; do
  "$UMBRIEL_UNMAP_CLIENT" "$title" 1200 700 > /dev/null 2>&1 &
  count=$((count + 1))
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq length) == "$count" ]] && break
    sleep 0.05
  done
done
[[ $("$UMBRIEL" windows --json | jq length) == 3 ]]

for workspace_axis in vertical horizontal; do
  strip_axis=horizontal
  strip_coordinate=x
  if [[ $workspace_axis == horizontal ]]; then
    strip_axis=vertical
    strip_coordinate=y
  fi
  first_position() {
    "$UMBRIEL" windows --json | jq -r --arg coordinate "$strip_coordinate" '.[] | select(.title == "touchpad-a") | .[$coordinate]'
  }

  configure "$workspace_axis" 1.0 1.0
  "$UMBRIEL" msg workspace-switch:1 > /dev/null
  "$UMBRIEL" msg column-focus-first > /dev/null
  "$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 360
  "$UMBRIEL" msg overview-open > /dev/null
  sleep 0.4

  # Travel across the workspace axis pans the strip. It keeps panning through drift onto the other axis, and never
  # activates another workspace. The client stays alive so the position can be read mid-gesture.
  before=$(first_position)
  "$UMBRIEL_POINTER_CLIENT" 1280 720 axis "$strip_axis" 150 axis "$workspace_axis" 80 pause 300 axis-stop "$strip_axis" &
  pointer_pid=$!
  moved=false
  for _ in $(seq 40); do
    if (( $(first_position) < before )); then moved=true; break; fi
    sleep 0.01
  done
  [[ $moved == true ]] || { echo "$workspace_axis: $strip_axis finger travel did not pan the preview"; exit 1; }
  [[ $(active_workspace) == 1 ]] || { echo "$workspace_axis: strip pan activated another workspace"; exit 1; }
  wait "$pointer_pid"

  # Travel along the workspace axis drags the filmstrip while held, and selects only at axis_stop.
  before_blue=$(blue "$workspace_axis-before")
  "$UMBRIEL_POINTER_CLIENT" 1280 720 axis "$workspace_axis" 210 pause 400 axis-stop "$workspace_axis" &
  pointer_pid=$!
  sleep 0.2
  [[ $(active_workspace) == 1 ]] || { echo "$workspace_axis: workspace committed before release"; exit 1; }
  during_blue=$(blue "$workspace_axis-during")
  (( before_blue > during_blue + 50 )) || {
    echo "$workspace_axis: filmstrip did not follow the fingers: blue $before_blue -> $during_blue"
    exit 1
  }
  wait "$pointer_pid"
  wait_workspace 2

  # The release settles rather than snapping: the filmstrip is still moving right after it, and at rest once the
  # spring is done. The settle does not depend on the arrangement, so one axis covers it.
  if [[ $workspace_axis == vertical ]]; then
    frame early
    sleep 0.15
    frame mid
    sleep 0.8
    frame settled
    sleep 0.15
    frame rest
    moving early mid || { echo "release snapped instead of settling: $(difference early mid)"; exit 1; }
    ! moving settled rest || { echo "filmstrip never came to rest: $(difference settled rest)"; exit 1; }
  fi

  # Losing the input device cancels the gesture; the same travel released properly commits.
  "$UMBRIEL_POINTER_CLIENT" 1280 720 axis "$workspace_axis" -210
  wait_workspace 2
  "$UMBRIEL_POINTER_CLIENT" 1280 720 axis "$workspace_axis" -210 pause 180 axis-stop "$workspace_axis"
  wait_workspace 1

  # Halving this physical axis halves its travel, whatever the workspace arrangement is.
  configure "$workspace_axis" 0.5 0.5
  "$UMBRIEL_POINTER_CLIENT" 1280 720 axis "$workspace_axis" 210 pause 180 axis-stop "$workspace_axis"
  sleep 0.3
  [[ $(active_workspace) == 1 ]] || { echo "$workspace_axis: halved scroll factor still selected a workspace"; exit 1; }

  # Travel means the same thing before the opening zoom has landed as it does after.
  if [[ $workspace_axis == vertical ]]; then
    configure "$workspace_axis" 1.0 1.0
    "$UMBRIEL" msg overview-close > /dev/null
    sleep 0.4
    "$UMBRIEL" msg overview-open > /dev/null
    "$UMBRIEL_POINTER_CLIENT" 1280 720 axis "$workspace_axis" 210 pause 180 axis-stop "$workspace_axis"
    wait_workspace 2
    "$UMBRIEL" msg workspace-switch:1 > /dev/null
  fi
  "$UMBRIEL" msg overview-close > /dev/null
  sleep 0.4
done

echo 'finger travel pans the strip, drags the filmstrip, settles on release, cancels with the device, and scales per axis'
