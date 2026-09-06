#!/usr/bin/env bash
# Finger axes move the overview before release, and select a workspace only at
# axis_stop. Removing the input device cancels an unfinished gesture.
set -euo pipefail

workspace_axis=${workspace_axis:-vertical}
strip_axis=horizontal
strip_coordinate=x
if [[ $workspace_axis == horizontal ]]; then
  strip_axis=vertical
  strip_coordinate=y
fi

pointer() { "$UMBRIEL_POINTER_CLIENT" 1280 720 "$@"; }
active_workspace() { "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .index'; }
first_position() { "$UMBRIEL" windows --json | jq -r --arg coordinate "$strip_coordinate" '.[] | select(.title == "touchpad-a") | .[$coordinate]'; }
wait_workspace() {
  for _ in $(seq 60); do
    [[ $(active_workspace) == "$1" ]] && return 0
    sleep 0.05
  done
  echo "expected workspace $1: $("$UMBRIEL" workspaces --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
enabled = false

[layout.scrolling]
default_width_fraction = 0.5

[output."HEADLESS-1"]
workspace_axis = "$workspace_axis"
EOF
"$UMBRIEL" msg config-reload > /dev/null
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
"$UMBRIEL" msg column-focus-first > /dev/null
pointer move 640 360
"$UMBRIEL" msg overview-open > /dev/null

# Keep the input device alive so position can be inspected mid-gesture. A
# strip drag must pan without selecting a different workspace, even with drift.
before=$(first_position)
"$UMBRIEL_POINTER_CLIENT" 1280 720 axis "$strip_axis" 150 axis "$workspace_axis" 80 pause 600 axis-stop "$strip_axis" &
pointer_pid=$!
moved=false
for _ in $(seq 40); do
  if (( $(first_position) < before )); then moved=true; break; fi
  sleep 0.01
done
[[ $moved == true ]] || { echo "$strip_axis finger input did not move the preview"; exit 1; }
[[ $(active_workspace) == 1 ]]
wait "$pointer_pid"

# Workspace selection must not commit while held.
grim "$UMBRIEL_RUNTIME_DIR/before-vertical.png"
before_blue=$(magick "$UMBRIEL_RUNTIME_DIR/before-vertical.png" -crop 20x20+630+350 -format '%[fx:round(255*mean.b)]' info:)
"$UMBRIEL_POINTER_CLIENT" 1280 720 axis "$workspace_axis" 210 pause 900 axis-stop "$workspace_axis" &
pointer_pid=$!
sleep 0.2
[[ $(active_workspace) == 1 ]] || { echo 'workspace committed before release'; exit 1; }
grim "$UMBRIEL_RUNTIME_DIR/during-vertical.png"
during_blue=$(magick "$UMBRIEL_RUNTIME_DIR/during-vertical.png" -crop 20x20+630+350 -format '%[fx:round(255*mean.b)]' info:)
(( before_blue > during_blue + 50 )) || { echo "row did not follow fingers: blue $before_blue -> $during_blue"; exit 1; }
wait "$pointer_pid"
wait_workspace 2

# Device removal is cancellation, not release/commit.
pointer axis "$workspace_axis" -210
wait_workspace 2
pointer axis "$workspace_axis" -210 pause 180 axis-stop "$workspace_axis"
wait_workspace 1

echo 'finger axes pan, lock their axis, settle on stop, and cancel on device removal'
