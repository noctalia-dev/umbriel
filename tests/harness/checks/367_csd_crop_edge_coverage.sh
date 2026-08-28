#!/usr/bin/env bash
# A client-side-decorated client hands over a surface larger than its window and
# leaves the difference transparent for its shadow. Cropping that surface to the
# window at a fractional scale lands the crop edge between texels, so the source
# box cannot be both snapped to whole texels (which sampling needs to stay sharp)
# and confined to the window. Snapping it outward samples the transparent margin,
# which draws one see-through line along the cropped edge: the window's own
# background shows through where its content should be.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/csd-crop-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/csd-crop.png"

# A pinned float fixes both content edges, and a one-logical-pixel margin at
# scale 5/4 puts the crop a quarter texel off the grid on every side.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[appearance]
border_width = 1
outer_border_width = 0
corner_radius = 12
border_focused = "#FF0000"
border_unfocused = "#FF0000"

[appearance.shadow]
enabled = false

[[window_rule]]
match.title = "^csd-crop$"
default_floating = true
default_size = [602, 402]
default_position = { x = 9, y = 9, anchor = "top_left" }

[output."HEADLESS-1"]
scale = 1.25
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" csd-crop 602 402 1 > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped ' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped ' "$CLIENT_LOG"; then
  echo "csd crop client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
sleep 0.3

read -r window_w window_h buffer_w surface_w margin < <(
  sed -n 's/^mapped \([0-9]*\)x\([0-9]*\) buffer \([0-9]*\)x[0-9]* surface \([0-9]*\)x[0-9]* margin \([0-9]*\) .*/\1 \2 \3 \4 \5/p' \
    "$CLIENT_LOG" | tail -1
)

window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "csd-crop")')
if [[ -z $window ]]; then
  echo "csd crop window missing from IPC"
  exit 1
fi
x=$(jq -r '.x' <<< "$window")
y=$(jq -r '.y' <<< "$window")
w=$(jq -r '.w' <<< "$window")
h=$(jq -r '.h' <<< "$window")
if (( w != window_w || h != window_h )); then
  echo "client did not adopt its configured window size: layout ${w}x${h}, client ${window_w}x${window_h}"
  exit 1
fi

# The crop must actually straddle texels, or the source box is already confined
# to the window and this check observes nothing.
if (( (margin * buffer_w) % surface_w == 0 )); then
  echo "crop landed on a texel boundary: margin $margin, buffer $buffer_w, surface $surface_w"
  exit 1
fi

grim -o HEADLESS-1 "$SCREENSHOT"

physical_round() {
  echo $(( ($1 * 5 + 2) / 4 ))
}
content_left=$(physical_round "$x")
content_top=$(physical_round "$y")
content_right=$(physical_round "$((x + w))")
content_bottom=$(physical_round "$((y + h))")
center_x=$(physical_round "$((x + w / 2))")
center_y=$(physical_round "$((y + h / 2))")

# Every window texel is one of two pure columns, and the margin is transparent.
# A pixel that is neither pure column means the draw sampled outside the window.
impure() {
  magick "$SCREENSHOT" -alpha off -crop "$1" +repage -depth 8 txt:- |
    tail -n +2 | grep -c -E -v '#(0000FF|00FF00) ' || true
}

first_column=$(impure "1x40+${content_left}+$((center_y - 20))")
last_column=$(impure "1x40+$((content_right - 1))+$((center_y - 20))")
first_row=$(impure "40x1+$((center_x - 20))+${content_top}")
last_row=$(impure "40x1+$((center_x - 20))+$((content_bottom - 1))")
if (( first_column != 0 || last_column != 0 || first_row != 0 || last_row != 0 )); then
  echo "cropped edges sampled outside the window: of 40 pixels, $first_column left, $last_column right," \
    "$first_row top and $last_row bottom are not window content"
  exit 1
fi

interior=$(impure "120x1+$((center_x - 60))+${center_y}")
if (( interior != 0 )); then
  echo "window columns were resampled: $interior of 120 interior pixels blend two columns"
  exit 1
fi

echo "cropped ${window_w}x${window_h} window inside a ${surface_w}-wide surface kept every edge pixel opaque and 1:1"
