#!/usr/bin/env bash
# A small configured radius describes the final outer edge. It must not grow by
# the total border width as the two color bands expand away from the content.
# Positive inner contours stay rounded, while zero preserves a square corner.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/small-border-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/small-border.png"
readonly TOTAL_WIDTH=9

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[colors.border]
focused = "#00FF00"
unfocused = "#00FF00"
outer = "#FF0000"

[appearance]
border_width = 1
outer_border_width = 8
corner_radius = 1
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" small-border 640 480 > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "small border client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
sleep 0.5

window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "small-border")')
if [[ -z $window ]]; then
  echo "small border client was not registered: $("$UMBRIEL" windows --json)"
  exit 1
fi
x=$(jq -r '.x' <<< "$window")
y=$(jq -r '.y' <<< "$window")

grim -o HEADLESS-1 "$SCREENSHOT"
sample() {
  magick "$SCREENSHOT" -alpha off -crop "1x1+$1+$2" +repage \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

corner=$(sample "$((x - TOTAL_WIDTH))" "$((y - TOTAL_WIDTH))")
edge=$(sample "$((x + 20))" "$((y - TOTAL_WIDTH))")
read -r corner_red corner_green corner_blue <<< "$corner"
read -r edge_red edge_green edge_blue <<< "$edge"
if (( corner_red < 180 || corner_green > 5 || corner_blue > 5 ||
      edge_red < 250 || edge_green > 1 || edge_blue > 1 )); then
  echo "one-pixel outer radius grew with the border: corner=$corner edge=$edge window=$window"
  exit 1
fi

echo "one-pixel outer radius stayed independent of border width: corner=$corner edge=$edge"

sed -i 's/^corner_radius = 1$/corner_radius = 8/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
sleep 0.1
grim -o HEADLESS-1 "$SCREENSHOT"
rounded_inner=$(sample "$x" "$y")
read -r rounded_inner_red rounded_inner_green rounded_inner_blue <<< "$rounded_inner"
if (( rounded_inner_red > 10 || rounded_inner_green < 220 || rounded_inner_blue > 10 )); then
  echo "positive inner radius collapsed to square: corner=$rounded_inner window=$window"
  exit 1
fi

echo "eight-pixel outer radius kept its inner contour rounded: corner=$rounded_inner"

sed -i 's/^corner_radius = 8$/corner_radius = 0/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
sleep 0.1
grim -o HEADLESS-1 "$SCREENSHOT"
square_corner=$(sample "$((x - TOTAL_WIDTH))" "$((y - TOTAL_WIDTH))")
read -r square_red square_green square_blue <<< "$square_corner"
if (( square_red < 250 || square_green > 1 || square_blue > 1 )); then
  echo "zero-radius border lost its square outer corner: corner=$square_corner window=$window"
  exit 1
fi

echo "zero-radius border kept its square outer corner: corner=$square_corner"
