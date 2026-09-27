#!/usr/bin/env bash
# The center of a scrolling card projected into the overview's margin must keep
# its content-anchored stack hint, not become the strip's prepend target at the
# centered preview boundary. That holds along either scrolling axis: with
# horizontally arranged workspaces the strip scrolls vertically and its cards
# overhang the preview's bottom edge instead of its left one.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly OVERVIEW_ZOOM=0.5
readonly OVERVIEW_X=320
readonly OVERVIEW_Y=180
# The zoom below scales the output to a 640x360 preview centred on both axes.
readonly PREVIEW_BOTTOM=540
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  foot --config=/dev/null --title="overhang-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_count() {
  for _ in $(seq 300); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.05
  done
  echo "timed out waiting for $1 window(s)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
insert_hint = "#FF0000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[layout.scrolling]
default_extent_fraction = 0.5

[overview]
zoom = 0.5

[output."HEADLESS-1"]
workspace_axis = "vertical"
EOF
"$UMBRIEL" msg config-reload > /dev/null
# Animation time only moves by clock-advance.
"$UMBRIEL" clock-freeze

# Runs every animation to its end. A resize crossfade starts only once its client commits, which happens in real time,
# so advance until a settle probe succeeds.
finish() {
  for _ in $(seq 20); do
    "$UMBRIEL" clock-advance 2000 > /dev/null
    if timeout 0.3 "$UMBRIEL" settle > /dev/null 2>&1; then
      return 0
    fi
  done
  echo "animations never finished: $("$UMBRIEL" windows --json)"
  return 1
}

for id in $(seq 1 7); do
  spawn_client "$id"
  wait_for_count "$id"
done
finish

# Keep the source column alive after detaching the dragged view so the strip's
# scroll range and the target card's projection remain stable.
"$UMBRIEL" msg window-consume-left > /dev/null
finish

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
finish
pointer_hold "$OUTPUT_W" "$OUTPUT_H" move "$start_x" "$start_y" press "$BTN_LEFT" move "$drop_x" "$drop_y" \
  -- release "$BTN_LEFT"
# Once the pointer holds at the drop point, animation time brings in the hint.
"$UMBRIEL" clock-advance 500 > /dev/null

screenshot="$UMBRIEL_RUNTIME_DIR/drag-overhanging-card.png"
grim "$screenshot"
red=$(magick "$screenshot" -crop "${sample_w}x50+${sample_x}+195" \
  -format '%[fx:round(255*mean.r)]' info:)
green=$(magick "$screenshot" -crop "${sample_w}x50+${sample_x}+195" \
  -format '%[fx:round(255*mean.g)]' info:)
pointer_release

if (( red < green + 35 )); then
  echo "the overhanging card center was replaced by the left-edge prepend target: red=$red green=$green"
  exit 1
fi

finish
"$UMBRIEL" msg overview-close > /dev/null
finish

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

# Arrange the workspaces horizontally: the strip now scrolls vertically, so later columns hang below the preview's
# bottom edge. A drop down there is only reachable because hit ownership extends across the output along the
# scrolling axis, and its projection must still name the column it points at.
sed -i 's/^workspace_axis = "vertical"$/workspace_axis = "horizontal"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

# overhang-7 shares its column with overhang-4, so detaching it cannot collapse a column and shift the stack under the
# pointer mid-drag. Focusing it also scrolls that column into the preview.
source_id=$(jq -r '.[] | select(.title == "overhang-7") | .id' <<< "$windows")
"$UMBRIEL" msg "window-focus:$source_id" > /dev/null
stack_rows=0
for _ in $(seq 30); do
  stack_rows=$("$UMBRIEL" windows --json | jq '[.[] | .y] | unique | length')
  ((stack_rows >= 5)) && break
  sleep 0.1
done
if ((stack_rows < 5)); then
  echo "the strip never stacked its columns vertically: $("$UMBRIEL" windows --json)"
  exit 1
fi

windows=$("$UMBRIEL" windows --json)
read -r source_x source_y source_h < <(
  jq -r '.[] | select(.title == "overhang-7") | "\(.x) \(.y) \(.h)"' <<< "$windows"
)
# The topmost column that starts below the preview's bottom edge: its card is visible only where it overhangs, so
# every point on it lies outside the preview box.
read -r target_title target_x target_top target_bottom < <(
  jq -r --argjson origin "$OVERVIEW_Y" --argjson zoom "$OVERVIEW_ZOOM" --argjson bottom "$PREVIEW_BOTTOM" '
    [.[] | {title, x, top: (($origin + .y * $zoom) | round), bottom: (($origin + (.y + .h) * $zoom) | round)}]
    | map(select(.top > $bottom))
    | min_by(.top)
    | "\(.title) \(.x) \(.top) \(.bottom)"' <<< "$windows"
)
if [[ $target_title == null || $target_title == "overhang-7" ]] \
  || ((target_top >= 640 || target_bottom <= target_top + 60)); then
  echo "test setup left no usable column overhanging below the preview: ${target_title} top=${target_top}: $windows"
  exit 1
fi
press_x=$((OVERVIEW_X + (source_x + 20) / 2))
press_y=$((OVERVIEW_Y + (source_y + source_h / 2) / 2))
# Drop near the column's leading cross edge so the stack target is its first row, and low enough that only the
# extended hit area can own the point.
drop_x=$((OVERVIEW_X + 100))
drop_y=$((target_top + 40))
if ((press_y <= OVERVIEW_Y + 5 || press_y >= 535)); then
  echo "test setup did not leave the drag source inside the preview: y=$press_y in $windows"
  exit 1
fi

"$UMBRIEL" msg overview-open > /dev/null
finish
pointer_hold "$OUTPUT_W" "$OUTPUT_H" move "$press_x" "$press_y" press "$BTN_LEFT" move "$drop_x" "$drop_y" \
  -- release "$BTN_LEFT"
"$UMBRIEL" clock-advance 500 > /dev/null

# The stack hint for the first row is a bar along the column's leading cross edge, projected into the overhanging
# part of the preview. The dragged card trails to the right of the pointer, so it cannot cover that bar.
vertical_shot="$UMBRIEL_RUNTIME_DIR/drag-overhanging-card-vertical.png"
grim "$vertical_shot"
hint_sample="45x30+$((OVERVIEW_X + target_x / 2 + 8))+$((target_top + 15))"
vertical_red=$(magick "$vertical_shot" -crop "$hint_sample" -format '%[fx:round(255*mean.r)]' info:)
vertical_green=$(magick "$vertical_shot" -crop "$hint_sample" -format '%[fx:round(255*mean.g)]' info:)
pointer_release

if ((vertical_red < vertical_green + 35)); then
  echo "no stack hint at the projected overhang position $hint_sample: red=$vertical_red green=$vertical_green"
  exit 1
fi

finish
"$UMBRIEL" msg overview-close > /dev/null
finish

windows=$("$UMBRIEL" windows --json)
read -r moved_x moved_y moved_h < <(
  jq -r '.[] | select(.title == "overhang-7") | "\(.x) \(.y) \(.h)"' <<< "$windows"
)
read -r landed_x landed_y landed_h < <(
  jq -r --arg title "$target_title" '.[] | select(.title == $title) | "\(.x) \(.y) \(.h)"' <<< "$windows"
)
if ((moved_y != landed_y || moved_h != landed_h || moved_x >= landed_x)); then
  echo "the drop below the preview did not stack overhang-7 before $target_title in its column: $windows"
  exit 1
fi

echo "the overhanging card center kept its stack hint and accepted the drop on either axis:" \
  "red=$red green=$green, overhang red=$vertical_red green=$vertical_green into $target_title"
