#!/usr/bin/env bash
# harness: outputs=1
# Floating maximize and restore must present intermediate position and size
# values through the shared windows_move animation.
set -euo pipefail

readonly TITLE=floating-maximize-animation
readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/floating-maximize-animation.log"

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
match.title = "^floating-maximize-animation$"
default_floating = true
default_floating_size_px = { width = 480, height = 300 }
default_position = { x = 173, y = 109, anchor = "top_left" }
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
  local label=$1 image="$UMBRIEL_RUNTIME_DIR/$1.png"
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

assert_exact_box() {
  local label=$1 x=$2 y=$3 width=$4 height=$5
  local expected_x=$6 expected_y=$7 expected_width=$8 expected_height=$9
  if (( x != expected_x || y != expected_y || width != expected_width || height != expected_height )); then
    echo "$label box was ${width}x${height}+${x}+${y}, expected ${expected_width}x${expected_height}+${expected_x}+${expected_y}"
    return 1
  fi
}

"$CLIENT" "$TITLE" 480 300 > "$CLIENT_LOG" 2>&1 &
wait_for_box 480x300+173+109
"$UMBRIEL" settle

read -r before_x before_y before_w before_h < <(capture_box before)
assert_exact_box before "$before_x" "$before_y" "$before_w" "$before_h" 173 109 480 300

# Animation time only moves by clock-advance: samples land at 150 ms and 450 ms of each 2000 ms timeline.
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg window-toggle-maximize > /dev/null
"$UMBRIEL" clock-advance 150
read -r max_x1 max_y1 max_w1 max_h1 < <(capture_box maximize-150ms)
"$UMBRIEL" clock-advance 300
read -r max_x2 max_y2 max_w2 max_h2 < <(capture_box maximize-450ms)

if ! (( 173 > max_x1 && max_x1 > max_x2 && max_x2 > 0
    && 109 > max_y1 && max_y1 > max_y2 && max_y2 > 0
    && 480 < max_w1 && max_w1 < max_w2 && max_w2 < 1280
    && 300 < max_h1 && max_h1 < max_h2 && max_h2 < 720 )); then
  echo "maximize did not interpolate monotonically: first=${max_w1}x${max_h1}+${max_x1}+${max_y1}, second=${max_w2}x${max_h2}+${max_x2}+${max_y2}"
  exit 1
fi

"$UMBRIEL" clock-advance 2000
"$UMBRIEL" settle
read -r maximized_x maximized_y maximized_w maximized_h < <(capture_box maximized)
assert_exact_box maximized "$maximized_x" "$maximized_y" "$maximized_w" "$maximized_h" 0 0 1280 720

"$UMBRIEL" msg window-toggle-maximize > /dev/null
"$UMBRIEL" clock-advance 150
read -r restore_x1 restore_y1 restore_w1 restore_h1 < <(capture_box restore-150ms)
"$UMBRIEL" clock-advance 300
read -r restore_x2 restore_y2 restore_w2 restore_h2 < <(capture_box restore-450ms)

if ! (( 0 < restore_x1 && restore_x1 < restore_x2 && restore_x2 < 173
    && 0 < restore_y1 && restore_y1 < restore_y2 && restore_y2 < 109
    && 1280 > restore_w1 && restore_w1 > restore_w2 && restore_w2 > 480
    && 720 > restore_h1 && restore_h1 > restore_h2 && restore_h2 > 300 )); then
  echo "restore did not interpolate monotonically: first=${restore_w1}x${restore_h1}+${restore_x1}+${restore_y1}, second=${restore_w2}x${restore_h2}+${restore_x2}+${restore_y2}"
  exit 1
fi

"$UMBRIEL" clock-advance 2000
"$UMBRIEL" settle
read -r restored_x restored_y restored_w restored_h < <(capture_box restored)
assert_exact_box restored "$restored_x" "$restored_y" "$restored_w" "$restored_h" 173 109 480 300

echo "floating maximize and restore animated position and size in both directions"
