#!/usr/bin/env bash
# harness: outputs=1
# The floating resize verbs animate the presented size, and the keep-visible
# clamp travels with that animation: a float hanging off the left edge shrinks
# without its origin snapping ahead of the size it is being clamped for.
set -euo pipefail

readonly TITLE=floating-resize-animation
readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/floating-resize-animation.log"

# 1000 wide at x=-900 keeps the 75px minimum on screen. Shrinking to 0.2 of the
# 1280 output (256px) tightens that minimum to 64px, so the origin has to travel
# to -192 and the visible sliver shrinks from 100px to 64px.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 2000
curve = "linear"

[animation.windows_in]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[[window_rule]]
match.title = "^floating-resize-animation$"
default_floating = true
default_size = [1000, 300]
default_position = { x = -900, y = 200, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

ipc_box() {
  "$UMBRIEL" windows --json \
    | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | "\(.w)x\(.h)+\(.x)+\(.y)"'
}

wait_for_box() {
  local expected=$1 actual=
  for _ in $(seq 100); do
    actual=$(ipc_box)
    [[ $actual == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected IPC box $expected, got $actual"
  return 1
}

capture_box() {
  local image="$UMBRIEL_RUNTIME_DIR/$1.png"
  local width height x y
  grim "$image"
  read -r width height x y < <(
    magick "$image" -alpha off -colorspace gray -threshold 1% \
      -bordercolor black -border 1 -trim -format '%w %h %X %Y\n' info:
  )
  x=${x#+}
  y=${y#+}
  printf '%d %d %d %d\n' "$((x - 1))" "$((y - 1))" "$width" "$height"
}

"$CLIENT" "$TITLE" 1000 300 > "$CLIENT_LOG" 2>&1 &
wait_for_box 1000x300+-900+200
sleep 0.2

read -r before_x before_y before_w before_h < <(capture_box before)
if (( before_x != 0 || before_y != 200 || before_w != 100 || before_h != 300 )); then
  echo "float did not open hanging off the left edge: ${before_w}x${before_h}+${before_x}+${before_y}"
  exit 1
fi

"$UMBRIEL" msg window-set-width:0.2 > /dev/null
sleep 0.25
read -r first_x first_y first_w first_h < <(capture_box shrink-250ms)
sleep 1.00
read -r second_x second_y second_w second_h < <(capture_box shrink-1250ms)

# The sliver stays between its start and end widths at every sample. An origin
# that ignores the requested size lets the float shrink off the output; an origin
# that snaps to it reveals the not yet shrunk buffer and blows past 100px; a
# snapped size lands on 64px before the animation has run.
if ! (( first_x == 0 && second_x == 0
    && first_y == 200 && second_y == 200
    && first_h == 300 && second_h == 300
    && 100 > first_w && first_w > second_w && second_w > 64 )); then
  echo "shrink did not keep origin and size in step: first=${first_w}x${first_h}+${first_x}+${first_y}, second=${second_w}x${second_h}+${second_x}+${second_y}"
  exit 1
fi

sleep 1.00
wait_for_box 256x300+-192+200
read -r after_x after_y after_w after_h < <(capture_box after)
if (( after_x != 0 || after_y != 200 || after_w != 64 || after_h != 300 )); then
  echo "float settled at ${after_w}x${after_h}+${after_x}+${after_y}, expected 64x300+0+200"
  exit 1
fi

echo "floating resize animated the size with the keep-visible clamp"
