#!/usr/bin/env bash
# The last scrolling column's append hint must remain outside the card when the
# pointer reaches the centered preview boundary.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly OVERVIEW_ZOOM=0.5
readonly OVERVIEW_X=320
readonly OVERVIEW_Y=180
readonly OVERVIEW_RIGHT=959
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  foot --config=/dev/null --override=colors.background=000000 \
    --title="right-edge-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
insert_hint_color = "#FF0000FF"
drag_opacity = 0.0

[layout.scrolling]
default_width_fraction = 0.5

[overview]
zoom = 0.5
background_tint = "#000000FF"
workspace_background = "#000000FF"
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

windows=$("$UMBRIEL" windows --json)
source_title=right-edge-5
focused_title=$(jq -r '.[] | select(.focused) | .title' <<< "$windows")
if [[ $focused_title != right-edge-6 ]]; then
  echo "expected the strip to be scrolled to its final column: $windows"
  exit 1
fi
start_x=$(jq -r --argjson origin "$OVERVIEW_X" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.title == "right-edge-5") | ($origin + ((.x + .w - 10) * $zoom) | round)' <<< "$windows")
start_y=$(jq -r --argjson origin "$OVERVIEW_Y" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.title == "right-edge-5") | ($origin + ((.y + .h / 2) * $zoom) | round)' <<< "$windows")

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.6
inside_x=$((OVERVIEW_RIGHT - 29))
pointer move "$start_x" "$start_y" press "$BTN_LEFT" \
  move "$OVERVIEW_RIGHT" 360 pause 1200 move "$inside_x" 360 pause 1200 release "$BTN_LEFT" &
pointer_pid=$!
sleep 0.5

outside_screenshot="$UMBRIEL_RUNTIME_DIR/drag-right-outside-hint.png"
grim "$outside_screenshot"
outside_red=$(magick "$outside_screenshot" -crop 100x240+980+240 -colorspace RGB \
  -format '%[fx:round(255*mean.r)]' info:)
outside_green=$(magick "$outside_screenshot" -crop 100x240+980+240 -colorspace RGB \
  -format '%[fx:round(255*mean.g)]' info:)
if (( outside_red < outside_green + 35 )); then
  echo "trailing gap hint overlapped the last card instead of staying in the overview margin: red=$outside_red green=$outside_green"
  exit 1
fi

sleep 1.2
inside_screenshot="$UMBRIEL_RUNTIME_DIR/drag-right-inside-hint.png"
grim "$inside_screenshot"
inside_red=$(magick "$inside_screenshot" -crop 100x50+700+195 -colorspace RGB \
  -format '%[fx:round(255*mean.r)]' info:)
inside_green=$(magick "$inside_screenshot" -crop 100x50+700+195 -colorspace RGB \
  -format '%[fx:round(255*mean.g)]' info:)
if (( inside_red < inside_green + 35 )); then
  echo "the last card's outer side remained a duplicate append target: red=$inside_red green=$inside_green"
  exit 1
fi

wait "$pointer_pid"
sleep 0.2
"$UMBRIEL" msg overview-close > /dev/null
sleep 0.6

windows=$("$UMBRIEL" windows --json)
read -r source_x source_y source_w < <(
  jq -r --arg title "$source_title" '.[] | select(.title == $title) | "\(.x) \(.y) \(.w)"' <<< "$windows"
)
read -r target_x target_y target_w < <(
  jq -r '.[] | select(.title == "right-edge-6") | "\(.x) \(.y) \(.w)"' <<< "$windows"
)
if (( source_x != target_x || source_w != target_w || source_y >= target_y )); then
  echo "dropping on the last card's outer side did not stack right-edge-5 above right-edge-6: $windows"
  exit 1
fi

echo "trailing gap stayed outside while the last card's outer side accepted a stack drop"
