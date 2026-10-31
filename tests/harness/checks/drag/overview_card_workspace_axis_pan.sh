#!/usr/bin/env bash
# harness: outputs=1
# Regression check (2026-10-27 user feedback: "the feature is missing for vertical workspace;
# horizontal and vertical scroll could use the same mechanic"): while an overview card drag is
# held, the touchpad finger-scroll stream must pan the WORKSPACE FILMSTRIP along the workspace axis
# — the workspace-axis twin of the strip pan drag/overview_card_axis_pan.sh pins. Pan-only: the
# rows follow the fingers, the active workspace never changes under the hold, and the release
# settles without selecting anything so the drop lands on the workspace under the pointer (the
# drop's focus is what activates its destination).
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

spawn_client() {
  foot --title="card-wsaxis-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .index'
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output."HEADLESS-1"]
workspaces = 3

[overview]
background_blur = false
shortcuts = false
zoom = 0.5
scroll_factor_horizontal = 1.0
scroll_factor_vertical = 1.0
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client 1
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.25
done
"$UMBRIEL" settle

active_before=$(active_workspace)
windows=$("$UMBRIEL" windows --json)
start_x=$(jq -r '.[] | select(.title == "card-wsaxis-1") | (.x + .w / 2 | round)' <<< "$windows")
start_y=$(jq -r '.[] | select(.title == "card-wsaxis-1") | (.y + .h / 2 | round)' <<< "$windows")
# Overview zoom 0.5: the active workspace preview is centred at origin 320,180, half scale; rows
# step 396 px (= 360 preview + 36 gap). The press lands at the card centre (~640,360 = row 0's band).
press_x=$((320 + start_x / 2))
press_y=$((180 + start_y / 2))

"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 360
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

# Drag threshold nudge is a pointer move (no pan); the axis stream is the touchpad finger-scroll.
# travelScale(1.0, zoom 0.5 -> zoomScale 1.25, factor 1.0, workspace 500) = 0.0025 rows/px, so
# 500 px vertical travel pans exactly one row: row 1's band (576..936 at rest) reaches the
# stationary pointer at y=360 and the drop must land on workspace 2.
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$press_x" "$press_y" press "$BTN_LEFT" move $((press_x + 30)) "$press_y" \
  -- axis vertical 500 pause 400 axis-stop vertical mark panned hold release "$BTN_LEFT"

pointer_step panned

# Pan-only contract: panning the rows under the hold must not step the filmstrip (switch the active
# workspace) before the drop resolves.
active_during=$(active_workspace)
if [[ $active_during != "$active_before" ]]; then
  echo "the finger pan switched the active workspace under the held card: $active_before -> $active_during"
  pointer_release
  exit 1
fi

pointer_release
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
dropped=$(jq -r '.[] | select(.title == "card-wsaxis-1") | .workspace' <<< "$windows")
focused=$(jq -r '.[] | select(.title == "card-wsaxis-1") | .focused // false' <<< "$windows")
echo "probe: dropped workspace $dropped (pointer at $((press_x + 30)),$press_y -> world $(((press_x + 30 - 320) * 2)),$(((press_y - 180) * 2)))"

if [[ $dropped != *":2" ]]; then
  echo "RED: finger travel during a held card drag did not pan the workspace filmstrip: drop stayed on $dropped"
  exit 1
fi

if [[ $focused != true ]]; then
  echo "the drop did not keep keyboard focus on the moved window: $windows"
  exit 1
fi

echo "GREEN: finger travel during a held card drag panned the filmstrip (drop on $dropped) without stepping the workspace"
