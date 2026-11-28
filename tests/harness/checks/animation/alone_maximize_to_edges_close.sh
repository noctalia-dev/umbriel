#!/usr/bin/env bash
# Closing the right tile makes the left tile alone: its rule-driven maximize-to-edges must animate into the vacancy.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/alone-edges-close.png"
readonly RIGHT_LOG="$UMBRIEL_RUNTIME_DIR/right.log"
msg() { "$UMBRIEL" msg "$@" > /dev/null; }

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = true
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
[animation.windows_move]
enabled = true
duration_ms = 1000
curve = "linear"
[animation.windows_resize]
enabled = true
duration_ms = 1000
curve = "linear"
wait_for_redraw = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.title_bar]
mode = "off"
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
[layout]
mode = "scrolling"
gap = 8
[layout.scrolling]
default_extent_fraction = 0.5
center_focused = "never"
center_underfull_strip = false
[[window_rule]]
match.title = "^alone-edges-left$"
match.is_alone = true
default_maximize_to_edges = true
CONFIG
msg config-reload

red_box() {
  grim -o HEADLESS-1 "$IMAGE"
  "$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'r > 0.9 && g < 0.1 && b < 0.1'
}

FILL_COLOR=0xFFFF0000 RESIZE_FILL_COLOR=0xFFFF0000 \
  "$UMBRIEL_UNMAP_CLIENT" alone-edges-left 1280 720 > "$UMBRIEL_RUNTIME_DIR/left.log" 2>&1 &
await_window alone-edges-left
"$UMBRIEL" settle
FILL_COLOR=0xFF0000FF RESIZE_FILL_COLOR=0xFF0000FF \
  "$UMBRIEL_UNMAP_CLIENT" alone-edges-right 1280 720 > "$RIGHT_LOG" 2>&1 &
await_window alone-edges-right
"$UMBRIEL" settle
read -r left_x _ before _ <<< "$(red_box)"
read -r right_x _ right_width _ <<< "$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.9 && r < 0.1 && g < 0.1')"
(( before > 0 && before < 980 && right_width > 0 && left_x + before <= right_x )) || {
  echo "expected two side-by-side tiles: left=$left_x width=$before right=$right_x width=$right_width"; exit 1;
}

"$UMBRIEL" clock-freeze
msg window-close "$(window_by_title alone-edges-right | jq -r .id)"
await_lines "$RIGHT_LOG" unmapped 1
for _ in $(seq 100); do
  window_by_title alone-edges-right > /dev/null || break
  sleep 0.025
done
if window_by_title alone-edges-right > /dev/null; then
  echo "the compositor did not observe the right tile closing"; exit 1;
fi

# Capture before advancing time as well as two intermediate frames: a snap on close followed by a resize animation
# would otherwise hide the initial jump. Only the survivor is red, and no closing snapshot masks its growing width.
read -r _ _ immediate _ <<< "$(red_box)"
(( immediate >= before - 2 && immediate <= before + 2 )) || {
  echo "the survivor jumped immediately on close: before=$before immediate=$immediate"; exit 1;
}
previous=$immediate
for elapsed in 250 500; do
  "$UMBRIEL" clock-advance 250
  read -r _ _ width _ <<< "$(red_box)"
  (( width > previous + 10 && width < 1270 )) || {
    echo "the survivor did not grow through an intermediate width at ${elapsed}ms: before=$before previous=$previous width=$width"
    exit 1
  }
  previous=$width
done
"$UMBRIEL" clock-advance 500
"$UMBRIEL" settle
read -r x y width height <<< "$(red_box)"
(( x == 0 && y == 0 && width == 1280 && height == 720 )) || {
  echo "the alone survivor did not fill the output edges: $x $y ${width}x${height}"; exit 1;
}

echo "closing the right tile animated the alone left tile from width $before to the output edges without an initial jump"
