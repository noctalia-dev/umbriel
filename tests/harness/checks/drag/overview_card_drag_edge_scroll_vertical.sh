#!/usr/bin/env bash
# harness: outputs=1
# Regression check (2026-10-27 user feedback, overview card drag input): the VERTICALLY scrolling
# strip twin of drag/overview_card_drag_edge_scroll.sh. With workspace_axis = "horizontal" the
# layout strip runs along Y (scrolling layouts run perpendicular to the workspace axis), and an
# overview card drag held at the bottom edge must slide it exactly like the horizontal twin slides
# the columns at the right edge — pinning that the edge auto-scroll mechanic is axis-generic.
# Asserts: post-detach baseline (beginDrag detaches the tile and the strip closes the gap live),
# >50 px of further vertical strip movement while parked at the edge, then drop placement after the
# revealed column.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

spawn_client() {
  foot --title="card-vedge-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

window_y() {
  jq -r --arg title "$1" '.[] | select(.title == $title) | .y' <<< "$2"
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output."HEADLESS-1"]
workspace_axis = "horizontal"

[layout.scrolling]
default_extent_fraction = 0.5
center_focused = "never"

[overview]
background_blur = false
shortcuts = false
zoom = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

for id in $(seq 1 4); do
  spawn_client "$id"
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $id ]] && break
    sleep 0.25
  done
done
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
initial_last_y=$(window_y card-vedge-4 "$windows")
echo "probe: initial card-vedge-4 y=$initial_last_y (strip must run along Y)"
if ((initial_last_y < OUTPUT_H)); then
  echo "the last column began inside the viewport instead of below it: $windows"
  exit 1
fi

"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 360
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

start_x=$(jq -r '.[] | select(.title == "card-vedge-1") | (.x + .w / 2 | round)' <<< "$windows")
start_y=$(jq -r '.[] | select(.title == "card-vedge-1") | (.y + .h / 2 | round)' <<< "$windows")
press_x=$((320 + start_x / 2))
press_y=$((180 + start_y / 2))

pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$press_x" "$press_y" press "$BTN_LEFT" move "$press_x" $((press_y + 20)) \
  -- move 640 719 mark atedge hold release "$BTN_LEFT"

windows=$("$UMBRIEL" windows --json)
base_last_y=$(window_y card-vedge-4 "$windows")
echo "probe: post-detach baseline card-vedge-4 y $initial_last_y -> $base_last_y"

pointer_step atedge

revealed=false
last_y=$base_last_y
for _ in $(seq 120); do
  windows=$("$UMBRIEL" windows --json)
  last_y=$(window_y card-vedge-4 "$windows")
  if ((last_y < base_last_y - 50)); then
    revealed=true
    break
  fi
  sleep 0.05
done
if [[ $revealed != true ]]; then
  echo "RED: overview card drag at the bottom edge did not slide the vertical strip: card-vedge-4 y $base_last_y -> $last_y"
  pointer_release
  exit 1
fi

pointer_release
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
moved_y=$(window_y card-vedge-1 "$windows")
dropped_last_y=$(window_y card-vedge-4 "$windows")
if ((moved_y <= dropped_last_y)); then
  echo "dropping at the revealed edge did not place card-vedge-1 after card-vedge-4: $windows"
  exit 1
fi

echo "GREEN: overview card drag at the bottom edge slid the vertical strip (card-vedge-4 y $base_last_y -> $last_y) and dropped after it"
