#!/usr/bin/env bash
# A button's symbol sits on the button's center at integer and fractional scales, and the button sits on the bar's
# center at integer scales.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"

# Blue carries the button's coverage (the symbol matches the idle button's blue) and red carries the symbol's.
cat >> "$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "forced"
height = 28
button_position = "right"
buttons = ["close"]
button_corner_radius = 0
button_glyph = "always"
[colors.title_bar]
background = "#00FF00FF"
background_unfocused = "#00FF00FF"
text = "#00FF00FF"
text_unfocused = "#00FF00FF"
close = "#0000FFFF"
close_glyph = "#FF00D9FF"
close_glyph_hover = "#FF00D9FF"
[output.HEADLESS-1]
scale = 1
[[window_rule]]
match.title = "^glyph-center$"
default_floating = true
default_floating_size_px = { width = 240, height = 140 }
default_position = { x = 101, y = 101, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFF000000 "$UMBRIEL_UNMAP_CLIENT" glyph-center 240 140 > "$UMBRIEL_RUNTIME_DIR/window.log" 2>&1 &
title_bar_wait_box glyph-center 240x140+101+101
image=$UMBRIEL_RUNTIME_DIR/glyph-center.png

for scale in 1 1.25 1.5 1.75 2; do
  for height in 28 29 30 31 32 33; do
    sed -i "s/^scale = .*/scale = $scale/; s/^height = .*/height = $height/" "$UMBRIEL_CONFIG"
    "$UMBRIEL" msg config-reload > /dev/null
    read -r output_w output_h < <("$UMBRIEL" outputs --json | jq -r '.[0].logical_size | "\(.width) \(.height)"')
    "$UMBRIEL_POINTER_CLIENT" "$output_w" "$output_h" move 0 0
    "$UMBRIEL" settle > /dev/null
    grim -o HEADLESS-1 "$image"
    read -r x y w h < <("$UMBRIEL_PIXEL_PROBE" "$image" bbox 'g > 0.5 && r < 0.5 && b < 0.5')
    bar="${w}x${h}+${x}+${y}"
    read -r button_x button_y < <("$UMBRIEL_PIXEL_PROBE" "$image" centroid b "$bar")
    read -r glyph_x glyph_y < <("$UMBRIEL_PIXEL_PROBE" "$image" centroid r "$bar")
    awk -v bx="$button_x" -v by="$button_y" -v gx="$glyph_x" -v gy="$glyph_y" \
      'BEGIN { exit !((gx - bx) ^ 2 <= 0.0225 && (gy - by) ^ 2 <= 0.0225) }' || {
      echo "scale=$scale height=$height: symbol center $glyph_x,$glyph_y is off the button center $button_x,$button_y"
      exit 1
    }
    # Fractional scales round each logical edge separately, so only integer scales promise equal bar margins.
    if [[ $scale == 1 || $scale == 2 ]]; then
      read -r cx cy cw ch < <("$UMBRIEL_PIXEL_PROBE" "$image" bbox 'b > 0.4' "$bar")
      ((cy - y == y + h - cy - ch && cy - y == x + w - cx - cw)) || {
        echo "scale=$scale height=$height: ${cw}x${ch} button margins are top $((cy - y)), bottom $((y + h - cy - ch)), right $((x + w - cx - cw))"
        exit 1
      }
    fi
  done
done
echo "button symbols stay centered at integer and fractional scales, and buttons on the bar at integer scales"
