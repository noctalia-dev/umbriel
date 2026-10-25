#!/usr/bin/env bash
# harness: outputs=1
# A floating window dragged mostly out of frame settles into the grabbable
# margin at release, animated rather than flashed. The margin is
# floatingKeepVisible: a quarter of the window, at least 10px and at most 75px,
# stays inside the usable area per axis.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly TITLE=release-settle
readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/release-settle.log"
readonly BTN_LEFT=272 # evdev BTN_LEFT
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

ipc_box() {
  "$UMBRIEL" windows --json | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | "\(.x) \(.y) \(.w) \(.h)"'
}

# The IPC box is the animation target; the presented sliver is what the user
# sees, so the interpolation is sampled from pixels.
capture_width() {
  local image="$UMBRIEL_RUNTIME_DIR/$1.png"
  local w h x y
  grim "$image"
  read -r w h x y < <(
    magick "$image" -alpha off -colorspace gray -threshold 1% \
      -bordercolor black -border 1 -trim -format '%w %h %X %Y\n' info:
  )
  echo "$w"
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation.windows_move]
duration_ms = 2000
curve = "linear"

[animation.windows_drag]
physics = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[[window_rule]]
match.title = "^release-settle$"
default_floating = true
default_floating_size_px = { width = 700, height = 700 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" "$TITLE" 700 700 > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  [[ -n $(ipc_box) ]] && break
  sleep 0.25
done
"$UMBRIEL" settle

# Grab near the right edge and shove the window to the left border: at the
# drop only 11px of the 700px window are still visible, and the margin (75px)
# asks for x=-625.
read -r wx wy ww wh < <(ipc_box)
gx=$((wx + ww - 6))
gy=$((wy + 20))
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  mod logo move "$gx" "$gy" press "$BTN_LEFT" move 5 "$gy" \
  -- release "$BTN_LEFT" mod none

# Freeze the clock before release so the settle is sampled halfway through its
# linear timeline: the visible sliver must sit strictly between the drop's 11px
# and the margin's 75px. A flash jumps straight to 75.
"$UMBRIEL" clock-freeze
pointer_release
w0=$(capture_width drop)
"$UMBRIEL" clock-advance 1000
w1=$(capture_width mid)
"$UMBRIEL" clock-advance 2000
w2=$(capture_width settled)
"$UMBRIEL" clock-resume

if ! ((w0 <= 12 && w0 < w1 && w1 < w2 && w2 >= 74 && w2 <= 76)); then
  echo "the release settle did not animate through the margin (slivers: $w0 -> $w1 -> $w2)"
  exit 1
fi

visible=$((ww / 4))
if ((visible < 10)); then visible=10; fi
if ((visible > 75)); then visible=75; fi
min_x=$((visible - ww))
final_x=$(ipc_box | cut -d' ' -f1)
if ((final_x < min_x)); then
  echo "the drop left the window out of frame: x=$final_x, margin allows x>=$min_x (box: $(ipc_box))"
  exit 1
fi
echo "a floating window settles into the margin at release, animated"
