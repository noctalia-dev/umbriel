#!/usr/bin/env bash
# The center of a scrolling card projected into the overview's left margin
# must keep its content-anchored stack hint, not become the strip's prepend
# target at the centered preview boundary.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly OVERVIEW_ZOOM=0.5
readonly OVERVIEW_X=320
readonly OVERVIEW_Y=180
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  foot --config=/dev/null --title="overhang-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_count() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
insert_hint_color = "#FF0000FF"

[layout.scrolling]
default_width_fraction = 0.5

[overview]
zoom = 0.5
background_tint = "#000000FF"
workspace_background = "#000000FF"
EOF
"$UMBRIEL" msg config-reload > /dev/null

for id in $(seq 1 7); do
  spawn_client "$id"
  wait_for_count "$id"
done
sleep 0.5

# Keep the source column alive after detaching the dragged view so the strip's
# scroll range and the target card's projection remain stable.
"$UMBRIEL" msg window-consume-left > /dev/null
sleep 0.6

windows=$("$UMBRIEL" windows --json)
source_column_x=$(jq -r '.[] | select(.title == "overhang-7") | .x' <<< "$windows")
source_sibling_x=$(jq -r '.[] | select(.title == "overhang-6") | .x' <<< "$windows")
if (( source_column_x != source_sibling_x )); then
  echo "test setup did not stack the drag source into the preceding column: $windows"
  exit 1
fi

start_x=$(jq -r --argjson origin "$OVERVIEW_X" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.title == "overhang-7") | ($origin + ((.x + .w - 10) * $zoom) | round)' <<< "$windows")
start_y=$(jq -r --argjson origin "$OVERVIEW_Y" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.title == "overhang-7") | ($origin + ((.y + .h / 2) * $zoom) | round)' <<< "$windows")
drop_x=$(jq -r --argjson origin "$OVERVIEW_X" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.title == "overhang-4") | ($origin + ((.x + .w / 2) * $zoom) | round)' <<< "$windows")
drop_y=$(jq -r --argjson origin "$OVERVIEW_Y" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.title == "overhang-4") | ($origin + ((.y + .h / 2) * $zoom) | round)' <<< "$windows")
target_right=$(jq -r --argjson origin "$OVERVIEW_X" --argjson zoom "$OVERVIEW_ZOOM" \
  '.[] | select(.title == "overhang-4") | ($origin + ((.x + .w) * $zoom) | round)' <<< "$windows")
if (( drop_x <= 32 || drop_x >= OVERVIEW_X )); then
  echo "test setup did not place the target card's center in the left overview margin: x=$drop_x"
  exit 1
fi
sample_x=$((drop_x + 30))
sample_w=$((target_right - sample_x - 20))
if (( sample_w < 40 )); then
  echo "test setup left too little uncovered target card for the hint sample: x=$drop_x right=$target_right"
  exit 1
fi

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.6
pointer move "$start_x" "$start_y" press "$BTN_LEFT" move "$drop_x" "$drop_y" pause 1500 release "$BTN_LEFT" &
pointer_pid=$!
sleep 0.5

screenshot="$UMBRIEL_RUNTIME_DIR/drag-overhanging-card.png"
grim "$screenshot"
red=$(magick "$screenshot" -crop "${sample_w}x50+${sample_x}+195" -colorspace RGB \
  -format '%[fx:round(255*mean.r)]' info:)
green=$(magick "$screenshot" -crop "${sample_w}x50+${sample_x}+195" -colorspace RGB \
  -format '%[fx:round(255*mean.g)]' info:)
wait "$pointer_pid"

if (( red < green + 35 )); then
  echo "the overhanging card center was replaced by the left-edge prepend target: red=$red green=$green"
  exit 1
fi

sleep 0.2
"$UMBRIEL" msg overview-close > /dev/null
sleep 0.6

windows=$("$UMBRIEL" windows --json)
read -r source_x source_y source_w < <(
  jq -r '.[] | select(.title == "overhang-7") | "\(.x) \(.y) \(.w)"' <<< "$windows"
)
read -r target_x target_y target_w < <(
  jq -r '.[] | select(.title == "overhang-4") | "\(.x) \(.y) \(.w)"' <<< "$windows"
)
if (( source_x != target_x || source_w != target_w || source_y >= target_y )); then
  echo "the overhanging card center did not stack overhang-7 above overhang-4: $windows"
  exit 1
fi

echo "the overhanging card center kept its stack hint and accepted the drop: red=$red green=$green"
