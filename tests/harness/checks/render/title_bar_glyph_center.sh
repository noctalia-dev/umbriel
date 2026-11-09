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

# button_glyph_size and button_glyph_width resize the symbol inside an unchanged button, and reload live.
sed -i "s/^scale = .*/scale = 1/; s/^height = .*/height = 32/" "$UMBRIEL_CONFIG"
sed -i '/^button_glyph = /a button_size = 24\nbutton_glyph_size = -1\nbutton_glyph_width = -1' "$UMBRIEL_CONFIG"
glyph_extent() {
  sed -i "s/^button_glyph_size = .*/button_glyph_size = $1/; s/^button_glyph_width = .*/button_glyph_width = $2/" \
    "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$image"
  read -r x y w h < <("$UMBRIEL_PIXEL_PROBE" "$image" bbox 'g > 0.5 && r < 0.5 && b < 0.5')
  read -r _ _ button_w _ < <("$UMBRIEL_PIXEL_PROBE" "$image" bbox 'b > 0.4' "${w}x${h}+${x}+${y}")
  read -r _ _ glyph_w _ < <("$UMBRIEL_PIXEL_PROBE" "$image" bbox 'r > 0.5' "${w}x${h}+${x}+${y}")
  echo "$button_w $glyph_w $("$UMBRIEL_PIXEL_PROBE" "$image" count 'r > 0.5' "${w}x${h}+${x}+${y}")"
}
read -r derived_button derived_w derived_ink < <(glyph_extent -1 -1)
read -r small_button small_w _ < <(glyph_extent 8 -1)
read -r _ _ thick_ink < <(glyph_extent -1 4)
((derived_button == 24 && small_button == 24)) || { echo "glyph settings changed the button: $derived_button, $small_button"; exit 1; }
# The derived X spans half the button; an 8 px one spans 8 px. Round caps add up to the stroke width.
((derived_w >= 12 && derived_w <= 15 && small_w >= 8 && small_w <= 11)) \
  || { echo "symbol widths: derived $derived_w, button_glyph_size = 8 gave $small_w"; exit 1; }
# Doubling the derived 2 px stroke; the crossing and caps overlap, so ink grows by less than double.
((thick_ink * 2 > derived_ink * 3)) || { echo "button_glyph_width = 4 drew $thick_ink pixels, derived $derived_ink"; exit 1; }
echo "button symbols stay centered at integer and fractional scales, and buttons on the bar at integer scales; glyph size and width resize the symbol"
