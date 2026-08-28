#!/usr/bin/env bash
# A toolkit that renders one buffer at the exact fractional output scale reports
# a pixel count that can be one short of the physical extent the compositor
# derives from the window's logical edges. Resolving that disagreement by
# shrinking the destination leaves an unpainted line inside the border's punched
# content hole, where the background shows through. The window must cover every
# pixel inside its content box, and its alternating columns must still land on
# exact texel centers, which is why the destination is not stretched instead.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/fractional-content-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/fractional-content.png"

# A pinned float fixes both content edges: the physical extent the compositor
# derives from them must round up while the client's floored buffer rounds down,
# or the one-pixel shortfall this check is about does not exist.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[appearance]
border_width = 1
outer_border_width = 0
corner_radius = 12
border_focused = "#FF0000"
border_unfocused = "#FF0000"

[[window_rule]]
match.title = "^fractional-content$"
default_floating = true
default_size = [602, 402]
default_position = { x = 9, y = 9, anchor = "top_left" }

[output."HEADLESS-1"]
scale = 1.25
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" fractional-content > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped ' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped ' "$CLIENT_LOG"; then
  echo "fractional content client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
sleep 0.3

# The client answers the unsized initial configure with its own size and only
# learns the layout's size once mapped, so the last report is the one that
# describes the window on screen.
read -r logical_w logical_h buffer_w buffer_h < <(
  sed -n 's/^mapped \([0-9]*\)x\([0-9]*\) buffer \([0-9]*\)x\([0-9]*\) .*/\1 \2 \3 \4/p' "$CLIENT_LOG" | tail -1
)

window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "fractional-content")')
if [[ -z $window ]]; then
  echo "fractional content window missing from IPC"
  exit 1
fi
x=$(jq -r '.x' <<< "$window")
y=$(jq -r '.y' <<< "$window")
w=$(jq -r '.w' <<< "$window")
h=$(jq -r '.h' <<< "$window")
if (( w != logical_w || h != logical_h )); then
  echo "client did not adopt its configured size: layout ${w}x${h}, client ${logical_w}x${logical_h}"
  exit 1
fi

# The headless mode is 1280x720 physical pixels at scale 5/4, so both content
# edges land on the rounding of a logical coordinate.
physical_round() {
  echo $(( ($1 * 5 + 2) / 4 ))
}
content_left=$(physical_round "$x")
content_top=$(physical_round "$y")
content_right=$(physical_round "$((x + w))")
content_bottom=$(physical_round "$((y + h))")
center_x=$(physical_round "$((x + w / 2))")
center_y=$(physical_round "$((y + h / 2))")
physical_w=$((content_right - content_left))
physical_h=$((content_bottom - content_top))

# Without the shortfall this check cannot observe anything: the buffer would
# already cover the content box, whatever the renderer does with the mismatch.
if (( physical_w != buffer_w + 1 || physical_h != buffer_h + 1 )); then
  echo "geometry lost the one-pixel shortfall: content ${physical_w}x${physical_h} at ${x},${y}" \
    "(${w}x${h} logical), buffer ${buffer_w}x${buffer_h}"
  exit 1
fi

grim -o HEADLESS-1 "$SCREENSHOT"

# Every client pixel is one of two pure columns. A background pixel means the
# content box was left uncovered; a blend of the two means the buffer was
# resampled instead of landing on texel centers.
impure() {
  magick "$SCREENSHOT" -alpha off -crop "$1" +repage -depth 8 txt:- |
    tail -n +2 | grep -c -E -v '#(0000FF|00FF00) ' || true
}

last_column=$(impure "1x40+$((content_right - 1))+$((center_y - 20))")
last_row=$(impure "40x1+$((center_x - 20))+$((content_bottom - 1))")
if (( last_column != 0 || last_row != 0 )); then
  echo "content box uncovered: $last_column of 40 pixels in the last column and $last_row of 40 in the last row" \
    "are not client content (content ${physical_w}x${physical_h} at ${content_left},${content_top})"
  exit 1
fi

interior=$(impure "120x1+$((center_x - 60))+${center_y}")
if (( interior != 0 )); then
  echo "client columns were resampled: $interior of 120 interior pixels blend two columns"
  exit 1
fi

echo "content ${physical_w}x${physical_h} covered by a ${buffer_w}x${buffer_h} buffer, columns still 1:1"
