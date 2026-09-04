#!/usr/bin/env bash
# A border must remain opaque when fractional output scaling maps its edges to
# physical pixels. Sampling a straight edge avoids corner coverage, so any
# background in the stripe is an antialiasing error rather than a curve.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/fractional-border-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/fractional-border.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[colors.border]
focused = "#FF0000"
unfocused = "#FF0000"

[appearance]
border_width = 1
outer_border_width = 0
corner_radius = 24

[output."HEADLESS-1"]
scale = 1.25
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" fractional-border 1024 576 > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "fractional border client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
sleep 0.3

window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "fractional-border")')
if [[ -z $window ]]; then
  echo "fractional border client was not registered: $("$UMBRIEL" windows --json)"
  exit 1
fi
x=$(jq -r '.x' <<< "$window")
y=$(jq -r '.y' <<< "$window")
w=$(jq -r '.w' <<< "$window")
h=$(jq -r '.h' <<< "$window")

# The headless mode is 1280x720 physical pixels. At scale 5/4, this
# deterministic geometry maps each straight one-logical-pixel edge to one
# physical pixel.
physical_round() {
  echo $(( ($1 * 5 + 2) / 4 ))
}
outer_x=$(physical_round "$((x - 1))")
inner_x=$(physical_round "$x")
outer_y=$(physical_round "$((y - 1))")
inner_y=$(physical_round "$y")
stripe_width=$((inner_x - outer_x))
stripe_height=$((inner_y - outer_y))
center_x=$(physical_round "$((x + w / 2))")
center_y=$(physical_round "$((y + h / 2))")
if (( stripe_width != 1 || stripe_height != 1 )); then
  echo "test geometry did not produce equal one-pixel edges: window=$window horizontal=$stripe_height vertical=$stripe_width"
  exit 1
fi

grim -o HEADLESS-1 "$SCREENSHOT"
side_red=$(magick "$SCREENSHOT" -alpha off -crop "1x100+${outer_x}+$((center_y - 50))" +repage \
  -format '%[fx:round(255*minima.r)]' info:)
top_red=$(magick "$SCREENSHOT" -alpha off -crop "100x1+$((center_x - 50))+${outer_y}" +repage \
  -format '%[fx:round(255*minima.r)]' info:)
if (( side_red < 254 || top_red < 254 )); then
  echo "fractionally scaled straight border was translucent: side_red=$side_red top_red=$top_red window=$window"
  exit 1
fi

# The ideal 1.25-pixel stroke has one opaque pixel plus equal fractional
# coverage outside its top and side edges. The rounded ring's transparent
# render margin keeps those samples inside the scene quad.
side_aa_red=$(magick "$SCREENSHOT" -alpha off -crop "1x100+$((outer_x - 1))+$((center_y - 50))" +repage \
  -format '%[fx:round(255*minima.r)]' info:)
top_aa_red=$(magick "$SCREENSHOT" -alpha off -crop "100x1+$((center_x - 50))+$((outer_y - 1))" +repage \
  -format '%[fx:round(255*minima.r)]' info:)
aa_delta=$((side_aa_red - top_aa_red))
(( aa_delta < 0 )) && aa_delta=$((-aa_delta))
if (( side_aa_red < 40 || side_aa_red > 90 || top_aa_red < 40 || top_aa_red > 90 || aa_delta > 2 )); then
  echo "fractional border outer coverage was uneven: side_aa=$side_aa_red top_aa=$top_aa_red window=$window"
  exit 1
fi

# The next row and column belong to the client. Antialiasing the stroke inward
# makes one edge look two pixels thick, depending on fractional rounding.
side_edge=$(magick "$SCREENSHOT" -alpha off -crop "1x1+${inner_x}+${center_y}" +repage \
  -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.b)]' info:)
top_edge=$(magick "$SCREENSHOT" -alpha off -crop "1x1+${center_x}+${inner_y}" +repage \
  -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.b)]' info:)
read -r side_edge_red side_edge_blue <<< "$side_edge"
read -r top_edge_red top_edge_blue <<< "$top_edge"
if (( side_edge_red > 90 || side_edge_blue < 160 || top_edge_red > 90 || top_edge_blue < 160 )); then
  echo "fractional border feathered into the straight client edge: side=$side_edge top=$top_edge window=$window"
  exit 1
fi

echo "fractional one-pixel border stayed opaque and even: side=$side_red+$side_aa_red top=$top_red+$top_aa_red"
