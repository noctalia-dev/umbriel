#!/usr/bin/env bash
# harness: outputs=1
# Regression check (2026-10-26 user feedback, overview card drag input): releasing the card mid-finger-pan
# must settle the in-flight scroll navigation pan-only and drop at the settled position. A navigation left
# armed past the drop takes the normal path afterwards (filmstrip step, focus-moving settle) and strands
# keyboard focus off the dropped window.
# Asserts: the strip pans with the drag held (>50 px, post-detach baseline), the drop lands under the
# stationary pointer at the panned position (world space), and the dropped window keeps keyboard focus.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

spawn_client() {
  foot --title="card-release-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
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
start_x=$(jq -r '.[] | select(.title == "card-release-1") | (.x + .w / 2 | round)' <<< "$windows")
start_y=$(jq -r '.[] | select(.title == "card-release-1") | (.y + .h / 2 | round)' <<< "$windows")
# Overview zoom 0.5: centered preview at origin 320,180, half scale.
press_x=$((320 + start_x / 2))
press_y=$((180 + start_y / 2))

"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 360
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

# Card drag (plain press past the drag threshold); the first hold lands after the threshold, so the
# post-detach position is the baseline. Then the finger pan runs and the button releases BEFORE the
# axis-stop — the drop must settle the navigation, not leave it armed.
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$press_x" "$press_y" press "$BTN_LEFT" move $((press_x + 30)) "$press_y" \
  -- axis horizontal 150 pause 200 mark panned hold release "$BTN_LEFT" axis-stop horizontal

before_x=$(window_x card-release-4 "$("$UMBRIEL" windows --json)")
pointer_step panned
after_x=$(window_x card-release-4 "$("$UMBRIEL" windows --json)")
if (( after_x >= before_x - 50 )); then
  echo "RED: axis travel during a held card drag did not pan the strip: card-release-4 x $before_x -> $after_x"
  pointer_release
  exit 1
fi

# The release runs the remaining steps (button release, then the axis-stop frame).
pointer_release
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
focused=$(jq -r '.[] | select(.focused == true) | .title' <<< "$windows")
moved_x=$(window_x card-release-1 "$windows")
moved_w=$(jq -r '.[] | select(.title == "card-release-1") | .w' <<< "$windows")
world_drop_x=$(((press_x + 30 - 320) * 2))

if [[ $focused != "card-release-1" ]]; then
  echo "RED: releasing mid-pan moved keyboard focus off the dropped window (focused: $focused): $windows"
  exit 1
fi
if (( world_drop_x < moved_x || world_drop_x >= moved_x + moved_w )); then
  echo "RED: the mid-pan drop did not land under the pointer: world x $world_drop_x outside card-release-1 x $moved_x..$((moved_x + moved_w)): $windows"
  exit 1
fi

echo "GREEN: releasing mid-pan settled the navigation (card-release-4 x $before_x -> $after_x), dropped under the pointer and kept focus"
