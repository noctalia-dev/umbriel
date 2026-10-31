#!/usr/bin/env bash
# harness: outputs=1
# Regression check (2026-10-26 user feedback, overview card drag input): an overview card drag
# (plain press on a window card) held at the screen edge of a long scrolling-layout workspace must
# slide the strip so the window can be dropped at the other end — the edge auto-scroll a tiled move
# already has (drag/move_drag_edge_scroll.sh), for the overview CARD drag.
# Asserts: post-detach baseline (beginDrag detaches the tile and the strip closes the gap live),
# >50 px of further strip movement while parked at the edge, then drop placement after the
# revealed column.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

spawn_client() {
  foot --title="card-edge-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

window_x() {
  jq -r --arg title "$1" '.[] | select(.title == $title) | .x' <<< "$2"
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

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
initial_last_x=$(window_x card-edge-4 "$windows")
if ((initial_last_x < OUTPUT_W)); then
  echo "the last column began inside the viewport instead of offscreen: $windows"
  exit 1
fi

"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 360
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

# Card position: overview zoom 0.5 scales the 1280x720 output to a centered preview
# (origin 320,180, half scale) — map the window's layout center onto it.
start_x=$(jq -r '.[] | select(.title == "card-edge-1") | (.x + .w / 2 | round)' <<< "$windows")
start_y=$(jq -r '.[] | select(.title == "card-edge-1") | (.y + .h / 2 | round)' <<< "$windows")
press_x=$((320 + start_x / 2))
press_y=$((180 + start_y / 2))

# Plain press = overview card drag (beginDrag at kDragThreshold), no modifier. The first hold
# lands after the drag threshold: beginDrag detaches the tile and the strip closes the gap, so
# the post-detach position is the baseline an edge timer must beat.
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$press_x" "$press_y" press "$BTN_LEFT" move $((press_x + 20)) "$press_y" \
  -- move 1279 360 mark atedge hold release "$BTN_LEFT"

windows=$("$UMBRIEL" windows --json)
base_last_x=$(window_x card-edge-4 "$windows")
echo "probe: post-detach baseline card-edge-4 x $initial_last_x -> $base_last_x"

pointer_step atedge

revealed=false
last_x=$base_last_x
for _ in $(seq 120); do
  windows=$("$UMBRIEL" windows --json)
  last_x=$(window_x card-edge-4 "$windows")
  if ((last_x < base_last_x - 50)); then
    revealed=true
    break
  fi
  sleep 0.05
done
if [[ $revealed != true ]]; then
  echo "RED: overview card drag at the right edge did not slide the strip: card-edge-4 x $base_last_x -> $last_x"
  pointer_release
  exit 1
fi

pointer_release
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
moved_x=$(window_x card-edge-1 "$windows")
dropped_last_x=$(window_x card-edge-4 "$windows")
if ((moved_x <= dropped_last_x)); then
  echo "dropping at the revealed edge did not place card-edge-1 after card-edge-4: $windows"
  exit 1
fi

echo "GREEN: overview card drag at the edge slid the strip (card-edge-4 x $base_last_x -> $last_x) and dropped after it"
