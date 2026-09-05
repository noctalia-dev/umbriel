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
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL_UNMAP_CLIENT" squash-example 700 700 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "squash-example")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
# Allow the initial configure/commit to settle before choosing edge samples.
sleep 0.15
window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "squash-example")')
x=$(jq -r '.x + (.w / 2 | floor)' <<< "$window")
top=$(jq -r '.y + 4' <<< "$window")
middle=$(jq -r '.y + (.h / 2 | floor)' <<< "$window")
bottom=$(jq -r '.y + .h - 6' <<< "$window")
blue_at() {
  magick "$IMAGE" -crop "2x2+$x+$1" -format '%[fx:round(mean.b*255)]' info:
}
sleep 0.85
grim "$IMAGE"
if (( $(blue_at "$top") > 10 || $(blue_at "$bottom") > 10 || $(blue_at "$middle") < 130 )); then
  echo "squash did not compress both edges while preserving the center"
  exit 1
fi
sleep 1.6
grim "$IMAGE"
if (( $(blue_at "$top") < 130 || $(blue_at "$bottom") < 130 || $(blue_at "$middle") < 130 )); then
  echo "squash did not restore edge and center pixels at x=$x y=$top,$middle,$bottom"
  exit 1
fi
echo "bundled squash compressed both edges and settled back to the original presentation"
