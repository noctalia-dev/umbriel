#!/usr/bin/env bash
# Border and surface shaders must use the same rounded-corner bounds. The client
# paints a full-window blue subsurface so a protruding corner is directly visible.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/subsurface-border-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/subsurface-border.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[colors.border]
focused = "#00FF00"
unfocused = "#00FF00"
outer = "#200000"

[appearance]
border_width = 1
outer_border_width = 8
corner_radius = 12
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" subsurface-border 640 480 animate > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "subsurface border client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
sleep 0.5

window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "subsurface-border")')
if [[ -z $window ]]; then
  echo "subsurface border client was not registered: $("$UMBRIEL" windows --json)"
  exit 1
fi
x=$(jq -r '.x' <<< "$window")
y=$(jq -r '.y' <<< "$window")

grim -o HEADLESS-1 "$SCREENSHOT"
sample() {
  magick "$SCREENSHOT" -alpha off -crop "1x1+$1+$2" +repage \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

# The configured 12 px outer radius stays fixed around both color bands. Smooth
# nesting keeps 7 px radii at the color seam and content edge.
outer_top=$(sample "$((x + 2))" "$((y - 9))")
outer_diagonal=$(sample "$((x - 6))" "$((y - 5))")
seam=$(sample "$((x + 4))" "$((y - 1))")
content_arc=$(sample "$((x + 3))" "$y")
inside=$(sample "$((x + 6))" "$y")
read -r outer_top_red outer_top_green outer_top_blue <<< "$outer_top"
read -r outer_diagonal_red outer_diagonal_green outer_diagonal_blue <<< "$outer_diagonal"
read -r seam_red seam_green seam_blue <<< "$seam"
read -r content_arc_red content_arc_green content_arc_blue <<< "$content_arc"
read -r inside_red inside_green inside_blue <<< "$inside"
if (( outer_top_red < 30 || outer_top_green > 1 || outer_top_blue > 1 ||
      outer_diagonal_red < 30 || outer_diagonal_green > 1 || outer_diagonal_blue > 1 ||
      seam_red > 10 || seam_green < 220 || seam_blue > 5 ||
      content_arc_red > 10 || content_arc_green < 220 || content_arc_blue > 10 ||
      inside_green > 10 || inside_blue < 240 )); then
  echo "smooth border contours were uneven: outer_top=$outer_top outer_diagonal=$outer_diagonal seam=$seam content_arc=$content_arc inside=$inside window=$window"
  exit 1
fi

echo "single-pass border kept smooth contours continuous: outer_top=$outer_top outer_diagonal=$outer_diagonal seam=$seam content_arc=$content_arc inside=$inside"
