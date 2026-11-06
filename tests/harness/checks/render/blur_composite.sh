#!/usr/bin/env bash
# Backdrop blur draws its final upsample straight into the target, applying the composite's alpha, rounded corners,
# transparency mask, and blending there. That must match compositing a stored full-resolution blur as a texture:
# `blur-composite-split` switches to the stored path at the same frozen instant, and the two screenshots may differ
# only by the rounding the skipped 8-bit buffer would add. The windows straddle the edges of a bright square, so the
# blur carries gradients, and the states cover a half-faded blur inside a fade capture, blur controls that push
# channels past 1, and the overview's output-wide background blur at partial strength and alpha while it opens.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly FUSED="$UMBRIEL_RUNTIME_DIR/blur-fused.png"
readonly SPLIT="$UMBRIEL_RUNTIME_DIR/blur-split.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 24

[appearance.shadow]
enabled = false

[appearance.blur]
enabled = true
optimized = false
passes = 2
radius = 6
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0

[animation]
duration_ms = 5000
curve = "linear"

[animation.windows_in]
style = "fade"

[animation.windows_move]
enabled = false

[animation.dim_unfocused]
enabled = false

[animation.overview]
curve = "linear"
duration_ms = 5000

[[window_rule]]
match.title = "^blur-clear$"
blur = true
default_floating = true
default_floating_size_px = { width = 500, height = 300 }
default_position = { x = 100, y = 100, anchor = "top_left" }

[[window_rule]]
match.title = "^blur-tinted$"
blur = true
blur_ignore_alpha = 0.3
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 150, y = 300, anchor = "top_left" }

[[window_rule]]
match.title = "^blur-masked$"
blur = true
blur_ignore_alpha = 0.5
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 700, y = 100, anchor = "top_left" }

[[window_rule]]
match.title = "^blur-bright$"
blur = true
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 50, y = 50, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

# A full-screen wallpaper with the layer client's 200x200 green square over its top-left corner.
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 > "$UMBRIEL_RUNTIME_DIR/wallpaper.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/wallpaper.log" ready 1
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 bottom-layer > "$UMBRIEL_RUNTIME_DIR/square.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/square.log" ready 1

spawn() {
  local title=$1 color=$2 width=$3 height=$4
  FILL_COLOR="$color" "$UMBRIEL_UNMAP_CLIENT" "$title" "$width" "$height" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  await_lines "$UMBRIEL_RUNTIME_DIR/$title.log" mapped 1
}

# Both screenshots come from a full redraw, so they share the same damage.
compare() {
  local state=$1 largest differing
  "$UMBRIEL" blur-composite-split off > /dev/null
  grim "$FUSED"
  "$UMBRIEL" blur-composite-split on > /dev/null
  grim "$SPLIT"
  "$UMBRIEL" blur-composite-split off > /dev/null
  read -r largest differing < <("$UMBRIEL_PIXEL_PROBE" "$FUSED" diff "$SPLIT")
  if ((largest > 1)); then
    echo "$state: direct blur composite differs from the stored one by up to $largest in $differing pixels"
    exit 1
  fi
}

"$UMBRIEL" clock-freeze
spawn blur-clear 0x00000000 500 300
spawn blur-tinted 0x80402010 300 200
spawn blur-masked 0x40102030 300 200
"$UMBRIEL" clock-advance 2350
# Halfway through the fade, the clear window's blur shows the square's edge as a gradient at half strength.
grim "$FUSED"
read -r _ green _ < <("$UMBRIEL_PIXEL_PROBE" "$FUSED" pixel 202 150)
if ((green < 125 || green > 150)); then
  echo "half-faded blur did not blend the square's edge: green=$green"
  exit 1
fi
compare "half-faded blur"
"$UMBRIEL" clock-advance 5000
compare "settled blur"

sed -i 's/^brightness = 1.0$/brightness = 0.7/; s/^contrast = 1.0$/contrast = 1.3/; s/^saturation = 1.0$/saturation = 0.5/' \
  "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
spawn blur-bright 0x00000000 300 200
"$UMBRIEL" clock-advance 2350
compare "half-faded blur with blur controls"
"$UMBRIEL" clock-advance 5000
compare "settled blur with blur controls"

"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" clock-advance 2000
compare "opening overview"
"$UMBRIEL" clock-advance 5000
compare "open overview"

echo "direct blur composite matches the stored composite through fades, blur controls, and the overview"
