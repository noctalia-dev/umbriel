#!/usr/bin/env bash
# harness: outputs=1
# A held overview card at the screen edge along the workspace axis slides the workspace rows with drag edge scroll.
# Asserts: a short slide followed by a drop back on the active workspace leaves the rows resting on that workspace
# again, and a long slide carries the card to the far workspace, past the last preview.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
# Card colours against the black overview background; workspace 3's window is red so its card is told apart.
readonly CARD='r > 0.08'
readonly RED='r > 0.3 && g < 0.1'

cat > "$UMBRIEL_CONFIG" <<EOF
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

[appearance.shadow]
enabled = false

[overview]
background_blur = false
shortcuts = false
zoom = 0.5

[output."HEADLESS-1"]
workspaces = 3
EOF
"$UMBRIEL" msg config-reload > /dev/null

# One window per workspace; the ones on 2 and 3 are the markers the slide brings into view.
count=0
for ws in 2 3 1; do
  "$UMBRIEL" msg workspace-switch "$ws" > /dev/null
  colour=$([[ $ws == 3 ]] && echo 800000 || echo 222222)
  foot --title="card-wsedge-$ws" -o "colors.background=$colour" sh -c 'sleep 120' > /dev/null 2>&1 &
  count=$((count + 1))
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $count ]] && break
    sleep 0.25
  done
done
"$UMBRIEL" settle

window_workspace() { "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "card-wsedge-1") | .workspace'; }

# Waits until pixels matching the predicate cover part of the region, sampled from live frames while the edge
# scroll runs.
await_card() {
  local frame=$UMBRIEL_RUNTIME_DIR/edge.png
  for _ in $(seq 200); do
    grim "$frame"
    (($("$UMBRIEL_PIXEL_PROBE" "$frame" count "$1" "$2") > 0)) && return 0
  done
  return 1
}

windows=$("$UMBRIEL" windows --json)
start_x=$(jq -r '.[] | select(.title == "card-wsedge-1") | (.x + .w / 2 | round)' <<< "$windows")
start_y=$(jq -r '.[] | select(.title == "card-wsedge-1") | (.y + .h / 2 | round)' <<< "$windows")
# The active preview sits at (320,180) at half scale; rows step 396 px along Y.
press_x=$((320 + start_x / 2))
press_y=$((180 + start_y / 2))

"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 360
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
grim "$UMBRIEL_RUNTIME_DIR/before.png"

# Short slide: workspace 2's card rises into the gap below the active preview, then the card drops back on workspace
# 1, which still covers y=200 at any offset reached here.
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$press_x" "$press_y" press "$BTN_LEFT" move $((press_x + 30)) "$press_y" \
  -- move 1000 719 mark short hold move 1000 200 release "$BTN_LEFT"
pointer_step short
await_card "$CARD" 20x10+630+556 || {
  echo "the bottom edge did not slide the workspace rows"
  exit 1
}
pointer_release
"$UMBRIEL" settle

[[ $(window_workspace) == *":1" ]] || {
  echo "the short slide's drop left workspace 1: $(window_workspace)"
  exit 1
}
grim "$UMBRIEL_RUNTIME_DIR/after.png"
read -r _ changed < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/after.png" diff "$UMBRIEL_RUNTIME_DIR/before.png" 640x720+0+0)
((changed == 0)) || {
  echo "the rows did not settle back on workspace 1 after the drop: $changed pixels differ"
  exit 1
}

# Long slide: hold until workspace 3's card reaches the middle of the screen, which puts the pointer past the last
# preview, then drop there.
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$press_x" "$press_y" press "$BTN_LEFT" move $((press_x + 30)) "$press_y" \
  -- move 1000 719 mark long hold release "$BTN_LEFT"
pointer_step long
await_card "$RED" 20x20+630+350 || {
  echo "the bottom edge did not slide the rows to workspace 3"
  exit 1
}
pointer_release
"$UMBRIEL" settle

[[ $(window_workspace) == *":3" ]] || {
  echo "the long slide did not drop on workspace 3: $(window_workspace)"
  exit 1
}

echo "edge scroll slid the workspace rows, settled them after a drop on workspace 1, and carried the card to 3"
