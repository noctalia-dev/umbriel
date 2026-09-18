#!/usr/bin/env bash
# harness: outputs=1
# A keybind resize of the focused scratchpad uses the ordinary windows_move
# presentation animation even though the scratchpad view has no workspace.
set -euo pipefail

readonly TITLE=scratchpad-resize-animation
readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-resize-animation.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 2000
curve = "linear"

[animation.windows_in]
enabled = false

[animation.scratchpad]
enabled = false
scale = 0

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[[window_rule]]
match.title = "^scratchpad-resize-animation$"
default_floating = true
default_floating_size_px = { width = 1000, height = 300 }
default_position = { x = 100, y = 200, anchor = "top_left" }
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
wait_for_box 1000x300+100+200

"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
wait_for_box 1000x300+100+200
"$UMBRIEL" msg scratchpad-toggle > /dev/null
sleep 0.2

read -r before_x before_y before_w before_h < <(capture_box before)
if ((before_x != 100 || before_y != 200 || before_w != 1000 || before_h != 300)); then
  echo "scratchpad did not open at 1000x300+100+200: ${before_w}x${before_h}+${before_x}+${before_y}"
  exit 1
fi

"$UMBRIEL" msg window-set-primary-extent:0.2 > /dev/null
sleep 0.25
read -r first_x first_y first_w first_h < <(capture_box resize-250ms)
sleep 1.00
read -r second_x second_y second_w second_h < <(capture_box resize-1250ms)

if ! ((first_x == 100 && second_x == 100
    && first_y == 200 && second_y == 200
    && first_h == 300 && second_h == 300
    && 1000 > first_w && first_w > second_w && second_w > 256)); then
  echo "scratchpad resize did not animate: first=${first_w}x${first_h}+${first_x}+${first_y}, second=${second_w}x${second_h}+${second_x}+${second_y}"
  exit 1
fi

sleep 1.00
wait_for_box 256x300+100+200
read -r after_x after_y after_w after_h < <(capture_box after)
if ((after_x != 100 || after_y != 200 || after_w != 256 || after_h != 300)); then
  echo "scratchpad resize settled at ${after_w}x${after_h}+${after_x}+${after_y}, expected 256x300+100+200"
  exit 1
fi

"$UMBRIEL" msg window-toggle-maximize > /dev/null
sleep 0.25
read -r max_first_x max_first_y max_first_w max_first_h < <(capture_box maximize-250ms)
sleep 1.00
read -r max_second_x max_second_y max_second_w max_second_h < <(capture_box maximize-1250ms)

if ! ((100 > max_first_x && max_first_x > max_second_x && max_second_x > 0
    && 200 > max_first_y && max_first_y > max_second_y && max_second_y > 0
    && 256 < max_first_w && max_first_w < max_second_w && max_second_w < 1280
    && 300 < max_first_h && max_first_h < max_second_h && max_second_h < 720)); then
  echo "scratchpad maximize did not animate: first=${max_first_w}x${max_first_h}+${max_first_x}+${max_first_y}, second=${max_second_w}x${max_second_h}+${max_second_x}+${max_second_y}"
  exit 1
fi

sleep 1.00
wait_for_box 1280x720+0+0
read -r max_after_x max_after_y max_after_w max_after_h < <(capture_box maximize-after)
if ((max_after_x != 0 || max_after_y != 0 || max_after_w != 1280 || max_after_h != 720)); then
  echo "scratchpad maximize settled at ${max_after_w}x${max_after_h}+${max_after_x}+${max_after_y}, expected 1280x720+0+0"
  exit 1
fi

echo "focused scratchpad size actions animated: resize ${first_w} -> ${second_w} -> ${after_w}, maximize ${max_first_w} -> ${max_second_w} -> ${max_after_w}"
