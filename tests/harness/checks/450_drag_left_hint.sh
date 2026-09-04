#!/usr/bin/env bash
# The first scrolling column's prepend hint must remain outside the card when
# the pointer crosses the centered preview boundary into the overview margin.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly OVERVIEW_ZOOM=0.5
readonly OVERVIEW_X=320
readonly OVERVIEW_Y=180
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

spawn_client() {
  foot --config=/dev/null --override=colors.background=000000 \
    --title="left-hint-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
insert_hint = "#FF0000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[appearance]
drag_opacity = 0.0

[layout.scrolling]
default_width_fraction = 0.5

[overview]
zoom = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

for id in $(seq 1 6); do
  spawn_client "$id"
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $id ]] && break
    sleep 0.25
  done
done
sleep 0.5

for _ in $(seq 1 5); do
  "$UMBRIEL" msg window-focus-left > /dev/null
done
sleep 0.5

windows=$("$UMBRIEL" windows --json)
start_x=$(jq -r --argjson origin "$OVERVIEW_X" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.focused) | ($origin + ((.x + 10) * $zoom) | round)' <<< "$windows")
start_y=$(jq -r --argjson origin "$OVERVIEW_Y" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.focused) | ($origin + ((.y + .h / 2) * $zoom) | round)' <<< "$windows")
# This maps within 32 world units of the normal viewport edge. The overview
# must still use strip content coordinates here instead of replacing the
# natural leading gap with an overlapping viewport-edge target.
touch_x=$((OVERVIEW_X - 13))
inside_x=$((OVERVIEW_X + 30))

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.6
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  move "$start_x" "$start_y" press "$BTN_LEFT" \
  move "$touch_x" 360 pause 1200 move "$inside_x" 360 pause 1200 release "$BTN_LEFT" &
POINTER_PID=$!
sleep 0.5

outside_screenshot="$UMBRIEL_RUNTIME_DIR/drag-left-outside-hint.png"
grim "$outside_screenshot"

outside_red=$(magick "$outside_screenshot" -crop 100x240+190+240 -colorspace RGB \
  -format '%[fx:round(255*mean.r)]' info:)
outside_green=$(magick "$outside_screenshot" -crop 100x240+190+240 -colorspace RGB \
  -format '%[fx:round(255*mean.g)]' info:)
if (( outside_red < outside_green + 35 )); then
  echo "leading gap hint overlapped the first card instead of staying in the overview margin: red=$outside_red green=$outside_green"
  exit 1
fi

sleep 1.2
inside_screenshot="$UMBRIEL_RUNTIME_DIR/drag-left-inside-hint.png"
grim "$inside_screenshot"

inside_red=$(magick "$inside_screenshot" -crop 100x50+400+195 -colorspace RGB \
  -format '%[fx:round(255*mean.r)]' info:)
inside_green=$(magick "$inside_screenshot" -crop 100x50+400+195 -colorspace RGB \
  -format '%[fx:round(255*mean.g)]' info:)
if (( inside_red < inside_green + 35 )); then
  echo "the first card's outer side remained a duplicate prepend target: red=$inside_red green=$inside_green"
  exit 1
fi

wait "$POINTER_PID"
sleep 0.2
"$UMBRIEL" msg overview-close > /dev/null
sleep 0.6

windows=$("$UMBRIEL" windows --json)
read -r source_x source_y source_w < <(
  jq -r '.[] | select(.title == "left-hint-1") | "\(.x) \(.y) \(.w)"' <<< "$windows"
)
read -r target_x target_y target_w < <(
  jq -r '.[] | select(.title == "left-hint-2") | "\(.x) \(.y) \(.w)"' <<< "$windows"
)
if (( source_x != target_x || source_w != target_w || source_y >= target_y )); then
  echo "dropping on the first card's outer side did not stack left-hint-1 above left-hint-2: $windows"
  exit 1
fi

echo "leading gap stayed outside while the first card's outer side accepted a stack drop"
