#!/usr/bin/env bash
# A tiled opener starts its windows_in in its final slot on the same tick established neighbours begin their
# windows_move reflow. Both clocks run independently: the opener is already visible while the neighbour is mid-reflow,
# and still mid-fade after that shorter reflow has settled. An opener that takes the whole width through
# default_maximize reflows them on that same clock.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/tiled-open-reflow.png"

# Keep most of the opener transparent during windows_in so the established tile stays observable behind it. A blue
# strip near the bottom exposes the opener's own progress before the fully blue client replaces it at completion.
cat > "$UMBRIEL_RUNTIME_DIR/open-marker.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
    if (uv.y < 0.8) {
        return vec4(0.0);
    }
    return vec4(0.0, 0.0, umbriel_clamped_progress, 1.0);
}
GLSL

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[layout]
mode = "master"

[animation.windows_in]
enabled = true
duration_ms = 1600
curve = "linear"
style = "none"
shader = "open-marker.glsl"

[animation.windows_out]
enabled = false

[animation.windows_move]
enabled = true
duration_ms = 600
curve = "linear"

[[window_rule]]
match.title = "^tiled-maximized-opener$"
default_maximize = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn() {
  local title=$1 color=$2
  FILL_COLOR="$color" "$UMBRIEL_UNMAP_CLIENT" "$title" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
    [[ -n $window ]] && return 0
    sleep 0.025
  done
  echo "timed out waiting for $title"
  return 1
}

# Only the survivor is red. Its pixel count on a middle row directly observes its presented width without relying on
# IPC target geometry or the opening window's opacity.
red_width() {
  grim "$IMAGE"
  local pixels
  pixels=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.8 && g < 0.1 && b < 0.1' 1280x8+0+356)
  echo $(((pixels + 4) / 8))
}

blue_at() {
  local x=$1 y=$2
  grim "$IMAGE"
  magick "$IMAGE" -alpha off -crop "8x8+$((x - 4))+$((y - 4))" +repage \
    -format '%[fx:round(255*mean.b)]\n' info:
}

# Animation time only moves by clock-advance: samples land at 300 ms (mid-reflow) and 900 ms (reflow done, windows_in
# at 0.56), and advancing 1600 ms finishes every timeline.
"$UMBRIEL" clock-freeze
spawn tiled-open-survivor 0xFFFF0000
"$UMBRIEL" clock-advance 1600
"$UMBRIEL" settle
before=$(red_width)

spawn tiled-opener 0xFF0000FF
opener=$window
# IPC has the final layout origin but may still expose the committed client size. Master places this opener against
# the output's right edge, so derive a point well inside its final slot from that edge.
opener_x=$(jq -r '.x + ((1280 - .x) / 2 | floor)' <<< "$opener")
opener_y=$(jq -r '.y + (((720 - .y) * 7 / 8) | floor)' <<< "$opener")
"$UMBRIEL" clock-advance 300
early=$(red_width)
early_blue=$(blue_at "$opener_x" "$opener_y")

"$UMBRIEL" clock-advance 600
mid=$(red_width)
mid_blue=$(blue_at "$opener_x" "$opener_y")

"$UMBRIEL" clock-advance 1600
"$UMBRIEL" settle
final=$(red_width)
final_blue=$(blue_at "$opener_x" "$opener_y")

if ((before - final < 300)); then
  echo "opening a second tile did not produce a measurable reflow: before=$before final=$final"
  exit 1
fi
if ((early <= final + 20 || early >= before - 20)); then
  echo "survivor was not mid-windows_move at 0.3 s: before=$before early=$early final=$final"
  exit 1
fi
if ! ((early_blue >= 15 && early_blue <= 110)); then
  echo "opener did not start windows_in alongside the neighbour reflow: $early_blue"
  exit 1
fi
if ((mid < final - 20 || mid > final + 20)); then
  echo "survivor had not settled on its own windows_move clock: mid=$mid final=$final"
  exit 1
fi
if ! ((mid_blue > early_blue + 40 && mid_blue <= 220 && final_blue > 220)); then
  echo "opener windows_in did not keep its own clock past the reflow: early=$early_blue mid=$mid_blue final=$final_blue"
  exit 1
fi

settled=$(red_width)
spawn tiled-maximized-opener 0xFF00FF00
"$UMBRIEL" clock-advance 300
maximized_early=$(red_width)
"$UMBRIEL" clock-advance 1600
"$UMBRIEL" settle
maximized_final=$(red_width)

if ((settled - maximized_final < 200)); then
  echo "the maximized opener did not take width from the survivor: ${settled} -> ${maximized_final}"
  exit 1
fi
if ((maximized_early <= maximized_final + 20 || maximized_early >= settled - 20)); then
  echo "the survivor snapped instead of reflowing under the maximized opener: ${settled} -> ${maximized_early} -> ${maximized_final}"
  exit 1
fi

echo "tiled opener ran windows_in in its final slot alongside the neighbour reflow, and a maximized opener reflowed that neighbour on windows_move too"
