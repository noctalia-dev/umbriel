#!/usr/bin/env bash
# harness: outputs=1
# Regression check (2026-10-27 user feedback: "the feature is missing for vertical workspace;
# horizontal and vertical scroll could use the same mechanic"): the workspace-axis twin of
# drag/overview_card_drag_edge_scroll.sh — an overview card drag held at the screen edge along the
# WORKSPACE axis slides the workspace filmstrip (input.drag_edge_scroll's zones/delay/ramp, reused
# from the layout-strip mechanic) so the window can be carried to the far workspace. Runs both
# workspace arrangements: the bottom edge with the default vertical axis, then the right edge with
# workspace_axis = "horizontal" (its layout strip turns vertical, see
# drag/overview_card_drag_edge_scroll_vertical.sh).
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

spawn_client() {
  foot --title="card-wsedge-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

# Whole-config rewrite (touchpad.sh's pattern): a second appended [output."HEADLESS-1"] table would
# be a duplicate TOML table and the reload would silently keep the old arrangement.
# $1 workspace axis.
configure() {
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
workspace_axis = "$1"
EOF
  "$UMBRIEL" msg config-reload > /dev/null
}

configure vertical

spawn_client 1
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.25
done
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
start_x=$(jq -r '.[] | select(.title == "card-wsedge-1") | (.x + .w / 2 | round)' <<< "$windows")
start_y=$(jq -r '.[] | select(.title == "card-wsedge-1") | (.y + .h / 2 | round)' <<< "$windows")
press_x=$((320 + start_x / 2))
press_y=$((180 + start_y / 2))

"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 360
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

# Park at the bottom edge (workspace axis is vertical; rows step 396 px at zoom 0.5). At rest the
# pointer at y=719 sits in row 1's band (576..936) — a plain drop lands on workspace 2. Only a
# filmstrip slide can bring workspace 3 (the far end) under the pointer.
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$press_x" "$press_y" press "$BTN_LEFT" move $((press_x + 30)) "$press_y" \
  -- move 640 719 mark atedge hold release "$BTN_LEFT"

pointer_step atedge

# The slide needs ~1.1s (vertical) / ~2.0s (horizontal) at max_speed 1500 after delay_ms 100;
# 3.5s covers both arrangements with margin.
for _ in $(seq 35); do
  sleep 0.1
done

pointer_release
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
dropped=$(jq -r '.[] | select(.title == "card-wsedge-1") | .workspace' <<< "$windows")
echo "probe: dropped workspace $dropped after parking at the bottom edge"

if [[ $dropped != *":3" ]]; then
  echo "RED: overview card drag at the bottom edge did not slide the workspace filmstrip to the far end: drop on $dropped"
  exit 1
fi

echo "GREEN: overview card drag at the bottom edge slid the filmstrip and dropped on $dropped"

# Phase 2 — the transposed arrangement. workspace_axis = "horizontal" steps the workspaces along X
# (and turns the layout strip vertical), so the same filmstrip slide must fire at the RIGHT edge.
configure horizontal
"$UMBRIEL" msg workspace-switch:1 > /dev/null
"$UMBRIEL" settle

spawn_client 2
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 2 ]] && break
  sleep 0.25
done
"$UMBRIEL" settle

# Phase 1's drop and the workspace switch can leave the overview closed; the drag needs it open.
"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 360
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
start_x=$(jq -r '.[] | select(.title == "card-wsedge-2") | (.x + .w / 2 | round)' <<< "$windows")
start_y=$(jq -r '.[] | select(.title == "card-wsedge-2") | (.y + .h / 2 | round)' <<< "$windows")
press_x=$((320 + start_x / 2))
press_y=$((180 + start_y / 2))

pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$press_x" "$press_y" press "$BTN_LEFT" move $((press_x + 30)) "$press_y" \
  -- move 1279 360 mark atedge2 hold release "$BTN_LEFT"

pointer_step atedge2

# The slide needs ~1.1s (vertical) / ~2.0s (horizontal) at max_speed 1500 after delay_ms 100;
# 3.5s covers both arrangements with margin.
for _ in $(seq 35); do
  sleep 0.1
done

pointer_release
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
dropped=$(jq -r '.[] | select(.title == "card-wsedge-2") | .workspace' <<< "$windows")
echo "probe: horizontal arrangement dropped workspace $dropped after parking at the right edge"

if [[ $dropped != *":3" ]]; then
  echo "RED: overview card drag at the right edge did not slide the filmstrip on the horizontal arrangement: drop on $dropped"
  exit 1
fi

echo "GREEN: overview card drag at the right edge slid the filmstrip and dropped on $dropped (horizontal arrangement)"
