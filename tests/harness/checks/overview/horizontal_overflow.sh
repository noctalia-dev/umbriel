#!/usr/bin/env bash
# Overview cards may extend beyond the scaled workspace preview while remaining inside the output, along whichever axis
# the workspaces scroll on: horizontally arranged workspaces scroll vertically, so their cards overhang the preview
# above and below it while the neighbouring workspace previews step along X.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/horizontal-overview-overflow.png"
readonly VERTICAL_SHOT="$UMBRIEL_RUNTIME_DIR/vertical-overview-overflow.png"
readonly BTN_LEFT=272
# The overview zoom below scales the 1280x720 output to a 640x360 preview centred on both axes.
readonly PREVIEW_X=320
readonly PREVIEW_Y=180
readonly PREVIEW_BOTTOM=540
readonly ZOOM=0.5
# Horizontally arranged workspaces step along X by the preview width plus a tenth of the output width, both zoomed.
readonly NEIGHBOUR_X=1024

spawn_client() {
  "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 &
}

sample_blue() {
  local image=$1 x=$2 y=$3
  magick "$image" -crop "20x20+$x+$y" -format '%[fx:round(255*mean.b)]' info:
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation.overview]
duration_ms = 1000
curve = "linear"

[colors]
backdrop = "#000000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[overview]
zoom = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client horizontal-overflow-first
wait_for_count 1
spawn_client horizontal-overflow-second
wait_for_count 2
spawn_client horizontal-overflow-third
wait_for_count 3

first_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "horizontal-overflow-first") | .id')
if [[ -z $first_id ]]; then
  echo "could not resolve the first horizontal column: $("$UMBRIEL" windows --json)"
  exit 1
fi
"$UMBRIEL" msg "window-focus:$first_id" > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
grim "$SCREENSHOT"
blue=$(magick "$SCREENSHOT" -crop 20x20+1090+350 -format '%[fx:round(255*mean.b)]' info:)
if (( blue < 80 )); then
  echo "horizontal card was clipped to the workspace background: blue=$blue"
  exit 1
fi

"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle

# Arrange the workspaces horizontally: the strip turns vertical, so the same three columns now stack downwards past the
# output's bottom edge instead of past its right edge.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output."HEADLESS-1"]
workspaces = 3
workspace_axis = "horizontal"
EOF
"$UMBRIEL" msg config-reload > /dev/null

# A client on the neighbouring workspace makes its preview identifiable by colour: the shared black workspace
# background cannot be told apart from the backdrop.
"$UMBRIEL" msg workspace-switch:2 > /dev/null
spawn_client horizontal-overflow-neighbour
wait_for_count 4
"$UMBRIEL" settle
"$UMBRIEL" msg workspace-switch:1 > /dev/null
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL" settle

strip_windows() {
  "$UMBRIEL" windows --json |
    jq '[.[] | select(.title | startswith("horizontal-overflow-")) | select(.title != "horizontal-overflow-neighbour")]'
}

vertical_rows=0
for _ in $(seq 30); do
  vertical_rows=$(strip_windows | jq '[.[] | .y] | unique | length')
  [[ $vertical_rows -eq 3 ]] && break
  sleep 0.1
done
if ((vertical_rows != 3)); then
  echo "the scrolling strip never stacked its columns vertically: $("$UMBRIEL" windows --json)"
  exit 1
fi

windows=$("$UMBRIEL" windows --json)
# A later column reaches past the output's bottom edge, so the only part of its card the overview can show is the
# stretch below the preview. Sample the middle of exactly that stretch.
read -r overhang_title overhang_x overhang_y < <(
  jq -r --argjson px "$PREVIEW_X" --argjson py "$PREVIEW_Y" --argjson bottom "$PREVIEW_BOTTOM" --argjson z "$ZOOM" '
    [.[] | select(.title | startswith("horizontal-overflow-")) | select(.title != "horizontal-overflow-neighbour")]
    | map({
        title: .title,
        x: (($px + (.x + .w / 2) * $z) | round),
        top: ([($py + .y * $z), $bottom + 5] | max),
        bottom: ([($py + (.y + .h) * $z), 715] | min),
      })
    | map(select(.bottom - .top >= 30))
    | first
    | "\(.title) \(.x) \(((.top + .bottom) / 2) | round)"' <<< "$windows"
)
if [[ -z ${overhang_y:-} ]] || ((overhang_y <= PREVIEW_BOTTOM + 15 || overhang_y >= 705)); then
  echo "test setup did not leave a card overhanging below the preview: y=${overhang_y:-none} in $windows"
  exit 1
fi
read -r neighbour_x neighbour_y < <(
  jq -r --argjson nx "$NEIGHBOUR_X" --argjson py "$PREVIEW_Y" --argjson z "$ZOOM" '
    .[] | select(.title == "horizontal-overflow-neighbour")
    | "\(($nx + (.x + 200) * $z) | round) \(($py + (.y + .h / 2) * $z) | round)"' <<< "$windows"
)
if ((neighbour_x <= 970 || neighbour_x >= 1250 || neighbour_y <= 190 || neighbour_y >= PREVIEW_BOTTOM - 15)); then
  echo "test setup did not project the neighbouring workspace beside the preview: x=$neighbour_x y=$neighbour_y"
  exit 1
fi

"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
grim "$VERTICAL_SHOT"

overhang_blue=$(sample_blue "$VERTICAL_SHOT" $((overhang_x - 10)) $((overhang_y - 10)))
if ((overhang_blue < 80)); then
  echo "the vertical strip's card was clipped to the workspace preview: blue=$overhang_blue at $overhang_x,$overhang_y"
  exit 1
fi

neighbour_blue=$(sample_blue "$VERTICAL_SHOT" $((neighbour_x - 10)) $((neighbour_y - 10)))
if ((neighbour_blue < 80)); then
  echo "the adjacent workspace preview did not step along X: blue=$neighbour_blue at $neighbour_x,$neighbour_y"
  exit 1
fi

# The overhanging card is still a hit target: clicking it below the preview activates its window and closes the
# overview.
"$POINTER" 1280 720 move "$overhang_x" "$overhang_y" press "$BTN_LEFT" release "$BTN_LEFT"
active=""
for _ in $(seq 30); do
  active=$("$UMBRIEL" windows --json | jq -r '.[] | select(.active) | .title')
  [[ $active == "$overhang_title" ]] && break
  sleep 0.1
done
if [[ $active != "$overhang_title" ]]; then
  echo "clicking the card overhanging below the preview did not activate $overhang_title, active is '$active'"
  exit 1
fi
if [[ $("$UMBRIEL" windows --json | jq 'length') -ne 4 ]]; then
  echo "the click below the preview did not land on a card: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "overview cards overhang the preview along the scrolling axis of either arrangement and stay clickable:" \
  "$overhang_title at $overhang_x,$overhang_y blue=$overhang_blue, neighbour blue=$neighbour_blue at" \
  "$neighbour_x,$neighbour_y"
