#!/usr/bin/env bash
# A window that opens fullscreen scales from the centre of its output, and its black backdrop scales with it instead
# of blanking the output for the whole windows_in timeline.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly PEER_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/fullscreen-open-center.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[animation]
duration_ms = 3000
curve = "linear"

[animation.windows_in]
enabled = false # OPEN_ANIMATION
style = "popin"
scale = 0.5

[animation.windows_move]
enabled = false

[animation.dim_unfocused]
enabled = false

[[window_rule]]
match.title = "^fullscreen-peer$"
default_floating = true
default_floating_size_px = { width = 120, height = 60 }
default_position = { x = 20, y = 20, anchor = "top_left" }

[[window_rule]]
match.title = "^fullscreen-open$"
default_fullscreen = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_window() {
  local title=$1
  for _ in $(seq 100); do
    if "$UMBRIEL" windows --json | jq -e --arg title "$title" '.[] | select(.title == $title)' > /dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "timed out waiting for $title"
  return 1
}

# Bounding box of the opener's red and green columns, which the fade leaves dim early in the timeline. The blue peer
# never matches.
opener_bounds() {
  grim "$IMAGE"
  "$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'r > 0.1 || g > 0.1'
}

peer_blue() {
  magick "$IMAGE" -format '%[fx:round(255*p{24,24}.b)]\n' info:
}

assert_centred() {
  local label=$1 x=$2 y=$3 w=$4 h=$5 low=$6 high=$7
  if ((w < low || w > high)); then
    echo "$label box was ${w}x${h}+${x}+${y}, expected ${low}-${high} wide"
    return 1
  fi
  local cx=$((2 * x + w)) cy=$((2 * y + h))
  if ((cx < 1268 || cx > 1292 || cy < 708 || cy > 732)); then
    echo "$label box ${w}x${h}+${x}+${y} was not centred on the output: 2cx=$cx 2cy=$cy"
    return 1
  fi
}

FILL_COLOR=0xFF0000FF "$PEER_CLIENT" fullscreen-peer 400 240 \
  > "$UMBRIEL_RUNTIME_DIR/fullscreen-peer.log" 2>&1 &
wait_for_window fullscreen-peer
"$UMBRIEL" settle

sed -i 's/^enabled = false # OPEN_ANIMATION$/enabled = true # OPEN_ANIMATION/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

# Animation time only moves by clock-advance; advancing 3000 ms finishes the 3000 ms opening timeline.
"$UMBRIEL" clock-freeze
"$CLIENT" fullscreen-open 400 240 > "$UMBRIEL_RUNTIME_DIR/fullscreen-open.log" 2>&1 &
wait_for_window fullscreen-open
"$UMBRIEL" clock-advance 900
read -r early_x early_y early_w early_h < <(opener_bounds)
early_blue=$(peer_blue)
assert_centred early "$early_x" "$early_y" "$early_w" "$early_h" 640 900

if ((early_blue < 200)); then
  echo "the floating peer was covered at 24,24 while the opener was ${early_w} wide: blue=$early_blue"
  exit 1
fi

"$UMBRIEL" clock-advance 900
read -r late_x late_y late_w late_h < <(opener_bounds)
assert_centred late "$late_x" "$late_y" "$late_w" "$late_h" 760 1150
if ((late_w < early_w + 120)); then
  echo "the opener did not keep growing: ${early_w} -> ${late_w}"
  exit 1
fi

"$UMBRIEL" clock-advance 3000
"$UMBRIEL" settle
read -r final_x final_y final_w final_h < <(opener_bounds)
# One edge column of the client's own pattern may fall outside the colour match, so allow a pixel either way.
if ((final_x > 1 || final_y > 1 || final_w < 1278 || final_h < 718)); then
  echo "the opener settled at ${final_w}x${final_h}+${final_x}+${final_y}, expected the whole 1280x720 output"
  exit 1
fi

echo "the fullscreen opener grew from the output centre (${early_w}x${early_h} -> ${late_w}x${late_h}) with its backdrop"
