#!/usr/bin/env bash
# harness: outputs=1
# Regression check (2026-10-26 user feedback, overview card drag input): while an overview card
# drag is held, smooth finger travel (the touchpad finger-scroll stream) must still pan the strip,
# as it does without a hold (overview/touchpad.sh) — the hold must not swallow the axis stream.
# Asserts: post-detach baseline (beginDrag detaches the tile and the strip closes the gap live),
# >50 px of strip pan from the axis travel with the drag held, and the drop lands after the panned
# position (the drop target refreshed under the stationary pointer).
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

spawn_client() {
  foot --title="card-axis-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
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
scroll_factor_horizontal = 1.0
scroll_factor_vertical = 1.0
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
start_x=$(jq -r '.[] | select(.title == "card-axis-1") | (.x + .w / 2 | round)' <<< "$windows")
start_y=$(jq -r '.[] | select(.title == "card-axis-1") | (.y + .h / 2 | round)' <<< "$windows")
# Overview zoom 0.5: centered preview at origin 320,180, half scale.
press_x=$((320 + start_x / 2))
press_y=$((180 + start_y / 2))

"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 360
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

# Card drag (plain press past the drag threshold). The first hold lands after the drag
# threshold — beginDrag detaches the tile and the strip closes the gap — so the post-detach
# position is the baseline the axis pan must beat. Then the touchpad finger-scroll stream runs
# while the drag is live (same travel overview/touchpad.sh pans with, without a hold).
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$press_x" "$press_y" press "$BTN_LEFT" move $((press_x + 30)) "$press_y" \
  -- axis horizontal 150 pause 400 axis-stop horizontal mark panned hold release "$BTN_LEFT"

before_x=$(window_x card-axis-4 "$("$UMBRIEL" windows --json)")
pointer_step panned
after_x=$(window_x card-axis-4 "$("$UMBRIEL" windows --json)")
pointer_release

if (( after_x >= before_x - 50 )); then
  echo "RED: axis travel during a held card drag did not pan the strip: card-axis-4 x $before_x -> $after_x"
  exit 1
fi

"$UMBRIEL" settle

# The release must drop under the stationary pointer at the settled panned position.
# (The drop resolves through workspace world space, the space windows --json reports.)
windows=$("$UMBRIEL" windows --json)
moved_x=$(window_x card-axis-1 "$windows")
moved_w=$(jq -r '.[] | select(.title == "card-axis-1") | .w' <<< "$windows")
# The pointer maps through the overview preview (origin 320,180, zoom 0.5) into workspace world
# space, which is the space windows --json reports.
world_drop_x=$(((press_x + 30 - 320) * 2))
if (( world_drop_x < moved_x || world_drop_x >= moved_x + moved_w )); then
  echo "the drop did not land under the pointer after the pan: world x $world_drop_x outside card-axis-1 x $moved_x..$((moved_x + moved_w)): $windows"
  exit 1
fi

echo "GREEN: axis travel during a held card drag panned the strip (card-axis-4 x $before_x -> $after_x) and the drop landed under the pointer"
