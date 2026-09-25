#!/usr/bin/env bash
# Tiled neighbours use windows_move timing while an opener or closing snapshot independently uses a longer lifecycle
# effect. By 350 ms the 150 ms reflow must already match its settled geometry in both directions. A window that
# rejoins the layout from floating reflows its neighbour without replaying its own windows_in.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/tiled-lifecycle-move-timing.png"

cat > "$UMBRIEL_RUNTIME_DIR/transparent.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return vec4(0.0); }
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
style = "fade"

[animation.windows_out]
enabled = true
duration_ms = 1600
curve = "linear"
style = "fade"
shader = "transparent.glsl"

[animation.windows_move]
enabled = true
duration_ms = 150
curve = "linear"
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

red_width() {
  grim "$IMAGE"
  local pixels
  pixels=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.8 && g < 0.1 && b < 0.1' 1280x8+0+356)
  echo $(((pixels + 4) / 8))
}

blue_pixels() {
  grim "$IMAGE"
  "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'b > 0.5 && r < 0.2'
}

# Animation time only moves by clock-advance. Advancing 1700 ms finishes every 1600 ms lifecycle effect.
"$UMBRIEL" clock-freeze
spawn lifecycle-move-survivor 0xFFFF0000
"$UMBRIEL" clock-advance 1700

# A transparent opener leaves the survivor fully observable even if broken code keeps it wide underneath the new slot.
spawn lifecycle-move-opener 0x00000000
opener_id=$(jq -r .id <<< "$window")
"$UMBRIEL" clock-advance 350
open_early=$(red_width)
"$UMBRIEL" clock-advance 1700
open_final=$(red_width)
if ((open_early < open_final - 20 || open_early > open_final + 20)); then
  echo "opening reflow followed windows_in instead of windows_move: early=$open_early final=$open_final"
  exit 1
fi

"$UMBRIEL" msg "window-close:$opener_id" > /dev/null
for _ in $(seq 80); do
  if ! "$UMBRIEL" windows --json | jq -e --arg id "$opener_id" 'any(.[]; .id == $id)' > /dev/null; then
    break
  fi
  sleep 0.025
done
"$UMBRIEL" clock-advance 350
close_early=$(red_width)
"$UMBRIEL" clock-advance 1700
close_final=$(red_width)
if ((close_early < close_final - 20 || close_early > close_final + 20)); then
  echo "closing reflow followed windows_out instead of windows_move: early=$close_early final=$close_final"
  exit 1
fi

# Re-tiling is not an admission: the returning window keeps what it shows while its neighbour reflows.
spawn lifecycle-move-peer 0xFF0000FF
"$UMBRIEL" clock-advance 1700
tiled_blue=$(blue_pixels)
if ((tiled_blue < 20000)); then
  echo "peer never settled as a visible tile: $tiled_blue"
  exit 1
fi
"$UMBRIEL" msg window-toggle-floating > /dev/null
"$UMBRIEL" clock-advance 1700
"$UMBRIEL" msg window-toggle-floating > /dev/null
"$UMBRIEL" clock-advance 150
retiled_early=$(blue_pixels)
"$UMBRIEL" clock-advance 250
retiled_late=$(blue_pixels)
if ((retiled_early < 20000 || retiled_late < 20000)); then
  echo "re-tiling replayed windows_in on a window that was already visible: early=$retiled_early late=$retiled_late"
  exit 1
fi

echo "tiled open, close, and re-tiling reflows used windows_move timing independently of lifecycle effects"
