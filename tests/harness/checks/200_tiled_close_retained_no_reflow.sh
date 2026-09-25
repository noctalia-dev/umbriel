#!/usr/bin/env bash
# A tiled close that started without layout reflow keeps its full captured mask when later layout changes happen
# elsewhere. A named-column opener and a subsequent consume both rearrange the left side while the close remains at
# the right for its independent windows_out lifetime.
set -euo pipefail

readonly SHOTS="$UMBRIEL_RUNTIME_DIR/tiled-close-retained-no-reflow"
readonly MOVE_MS=500
mkdir -p "$SHOTS"

cat > "$UMBRIEL_RUNTIME_DIR/solid-blue.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return vec4(0.0, 0.0, 1.0, 1.0); }
GLSL

cat >> "$UMBRIEL_CONFIG" <<EOF

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[layout]
mode = "scrolling"
gap = 0

[layout.scrolling]
default_extent_fraction = 0.3
center_focused = "never"
center_underfull_strip = false

[animation.windows_in]
enabled = false

[animation.windows_out]
enabled = true
duration_ms = 6500
curve = "linear"
shader = "solid-blue.glsl"

[animation.windows_move]
enabled = true
duration_ms = $MOVE_MS
curve = "linear"

[[window_rule]]
match.title = "^retained-owner$"
default_scrolling_column = "retained-left-stack"
default_scrolling_column_order = 10

[[window_rule]]
match.title = "^retained-opener$"
default_scrolling_column = "retained-left-stack"
default_scrolling_column_order = 20
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn() {
  local title=$1 color=$2
  FILL_COLOR="$color" "$UMBRIEL_UNMAP_CLIENT" "$title" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 100); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
    [[ -n $window ]] && return 0
    sleep 0.025
  done
  echo "timed out waiting for $title"
  return 1
}

wait_unmapped() {
  local title=$1
  for _ in $(seq 100); do
    if grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/$title.log" \
        && ! "$UMBRIEL" windows --json | jq -e --arg title "$title" 'any(.[]; .title == $title)' > /dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "timed out waiting for $title to unmap"
  return 1
}

wait_for_query() {
  local query=$1 message=$2 windows=
  for _ in $(seq 100); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e "$query" <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "$message: $windows"
  return 1
}

capture_series() {
  local phase=$1
  for i in $(seq 0 8); do
    grim "$SHOTS/$phase-$i.png"
    sleep 0.08
  done
}

blue_pixels() {
  "$UMBRIEL_PIXEL_PROBE" "$1" count 'b > 0.8 && r < 0.1 && g < 0.1'
}

blue_bounds() {
  "$UMBRIEL_PIXEL_PROBE" "$1" bbox 'b > 0.8 && r < 0.1 && g < 0.1'
}

bounds_match() {
  local tolerance=$1
  local ax=$2 ay=$3 aw=$4 ah=$5
  local bx=$6 by=$7 bw=$8 bh=$9
  ((ax >= bx - tolerance && ax <= bx + tolerance \
    && ay >= by - tolerance && ay <= by + tolerance \
    && aw >= bw - tolerance && aw <= bw + tolerance \
    && ah >= bh - tolerance && ah <= bh + tolerance))
}

spawn retained-owner 0xFFFF0000
"$UMBRIEL" settle
spawn retained-peer 0xFFFF0000
peer_id=$(jq -r .id <<< "$window")
"$UMBRIEL" settle
spawn retained-close 0xFF0000FF
closing_id=$(jq -r .id <<< "$window")
"$UMBRIEL" settle

survivors_before=$("$UMBRIEL" windows --json | jq -c \
  '[.[] | select(.title == "retained-owner" or .title == "retained-peer") | {title, x, y, w, h}] | sort_by(.title)')
"$UMBRIEL" msg "window-close:$closing_id" > /dev/null
wait_unmapped retained-close
survivors_after=$("$UMBRIEL" windows --json | jq -c \
  '[.[] | select(.title == "retained-owner" or .title == "retained-peer") | {title, x, y, w, h}] | sort_by(.title)')
if [[ $survivors_after != "$survivors_before" ]]; then
  echo "setup close unexpectedly reflowed its survivors: before=$survivors_before after=$survivors_after"
  exit 1
fi

grim "$SHOTS/baseline.png"

# Joining the owner's named column changes only the left column's row geometry. Capture the whole windows_move
# transition before doing image analysis so processing time cannot hide a transient close-mask collapse.
spawn retained-opener 0xFFFF0000
capture_series opening
wait_for_query \
  '[.[] | select(.title == "retained-owner" or .title == "retained-opener")] as $stack
    | ($stack | length == 2)
      and ([$stack[].x] | unique | length == 1)
      and ([$stack[].y] | unique | length == 2)' \
  "named opener did not split the left column"

# Consuming the middle peer adds a third row to the same left column. It must not reinterpret the retained close as a
# new vacancy or collapse its mask.
"$UMBRIEL" msg "window-focus:$peer_id" > /dev/null
"$UMBRIEL" msg window-consume-left > /dev/null
capture_series consume
wait_for_query \
  '[.[] | select(.title == "retained-owner" or .title == "retained-opener" or .title == "retained-peer")] as $stack
    | ($stack | length == 3)
      and ([$stack[].x] | unique | length == 1)
      and ([$stack[].y] | unique | length == 3)' \
  "consume did not move the middle peer into the left column"

baseline_pixels=$(blue_pixels "$SHOTS/baseline.png")
baseline_bounds=$(blue_bounds "$SHOTS/baseline.png")
if ((baseline_pixels < 10000)); then
  echo "no-reflow close did not retain a measurable baseline mask: pixels=$baseline_pixels bounds=$baseline_bounds"
  exit 1
fi
read -r base_x base_y base_width base_height <<< "$baseline_bounds"

for phase in opening consume; do
  for i in $(seq 0 8); do
    image="$SHOTS/$phase-$i.png"
    pixels=$(blue_pixels "$image")
    bounds=$(blue_bounds "$image")
    if ((pixels < baseline_pixels - 100 || pixels > baseline_pixels + 100)); then
      echo "$phase frame $i changed the retained close-mask area: baseline=$baseline_pixels current=$pixels"
      exit 1
    fi
    read -r x y width height <<< "$bounds"
    if ! bounds_match 2 "$x" "$y" "$width" "$height" \
        "$base_x" "$base_y" "$base_width" "$base_height"; then
      echo "$phase frame $i moved or reshaped the retained close mask: baseline=$baseline_bounds current=$bounds"
      exit 1
    fi
  done
done

echo "a retained no-reflow close stayed fixed through an unrelated named opening and consume"
