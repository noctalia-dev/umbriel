#!/usr/bin/env bash
# Hold layout still while the opening timeline exercises the bundled shader.
# Its middle frame must compress the client, then restore the same edge pixels.
set -euo pipefail
readonly SHADER="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../examples/shaders" && pwd)/squash.glsl"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/squash.png"
cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
duration_ms = 2400
curve = "linear"
[animation.windows_in]
style = "none"
shader = "$SHADER"
[animation.windows_move]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[colors]
shadow = "#00FF00FF"
[appearance.shadow]
enabled = true
softness = 24
offset_x = 0
offset_y = 0
EOF
"$UMBRIEL" msg config-reload > /dev/null
# Animation time only moves by clock-advance: the middle sample lands at 1000 ms of the 2400 ms timeline.
"$UMBRIEL" clock-freeze
"$UMBRIEL_UNMAP_CLIENT" squash-example 700 700 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "squash-example")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
"$UMBRIEL" clock-advance 150
window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "squash-example")')
x=$(jq -r '.x + (.w / 2 | floor)' <<< "$window")
top=$(jq -r '.y + 4' <<< "$window")
middle=$(jq -r '.y + (.h / 2 | floor)' <<< "$window")
bottom=$(jq -r '.y + .h - 6' <<< "$window")
blue_at() {
  magick "$IMAGE" -crop "2x2+$x+$1" -format '%[fx:round(mean.b*255)]' info:
}
"$UMBRIEL" clock-advance 850
grim "$IMAGE"
if (( $(blue_at "$top") > 10 || $(blue_at "$bottom") > 10 || $(blue_at "$middle") < 130 )); then
  echo "squash did not compress both edges while preserving the center"
  exit 1
fi
for edge in "$top" "$bottom"; do
  green=$(magick "$IMAGE" -crop "2x2+$x+$edge" -format '%[fx:round(mean.g*255)]' info:)
  if (( green < 25 )); then
    echo "squash shadow did not follow the compressed edge at y=$edge: green=$green"
    exit 1
  fi
done
"$UMBRIEL" clock-advance 2400
"$UMBRIEL" settle
grim "$IMAGE"
if (( $(blue_at "$top") < 130 || $(blue_at "$bottom") < 130 || $(blue_at "$middle") < 130 )); then
  echo "squash did not restore edge and center pixels at x=$x y=$top,$middle,$bottom"
  exit 1
fi
echo "bundled squash and bright shadows compressed both edges and restored together"
