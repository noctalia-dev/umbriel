#!/usr/bin/env bash
# Each button uses its own idle and hover glyph colors, with optional visibility only while hovered or pressed.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"

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
height = 40
button_position = "right"
button_glyph = "hover"
button_background = true
[colors.title_bar]
background = "#000000FF"
background_unfocused = "#000000FF"
text = "#000000FF"
text_unfocused = "#000000FF"
close = "#303030FF"
maximize = "#303030FF"
minimize = "#303030FF"
close_glyph = "#FF0000FF"
close_glyph_hover = "#00FF00FF"
maximize_glyph = "#0000FFFF"
maximize_glyph_hover = "#FF00FFFF"
minimize_glyph = "#FFFF00FF"
minimize_glyph_hover = "#00FFFFFF"
[input.focus]
follows_mouse = false
[output.HEADLESS-1]
scale = 1.25
[[window_rule]]
match.title = "^glyph-hover$"
default_floating = true
default_floating_size_px = { width = 240, height = 140 }
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFF000000 "$UMBRIEL_UNMAP_CLIENT" glyph-hover 240 140 > "$UMBRIEL_RUNTIME_DIR/window.log" 2>&1 &
title_bar_wait_box glyph-hover 240x140+100+100
title_bar_wait_field glyph-hover minimizable true
read -r output_w output_h < <("$UMBRIEL" outputs --json | jq -r '.[0].logical_size | "\(.width) \(.height)"')
image=$UMBRIEL_RUNTIME_DIR/glyph-hover.png
idle=('r > 0.4 && g > 0.4 && b < 0.2' 'b > 0.4 && r < 0.2 && g < 0.2' 'r > 0.4 && g < 0.2 && b < 0.2')
hover=('g > 0.4 && b > 0.4 && r < 0.2' 'r > 0.4 && b > 0.4 && g < 0.2' 'g > 0.4 && r < 0.2 && b < 0.2')

capture_at() {
  "$UMBRIEL_POINTER_CLIENT" "$output_w" "$output_h" move "$1" "$2"
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$image"
}
assert_glyphs() {
  local active=$1 index region expected count other
  for index in 0 1 2; do
    region=$(awk -v x="$((254 + index * 28))" 'BEGIN { printf "25x25+%d+87", x*1.25 }')
    expected=${idle[index]}
    if ((index == active)); then expected=${hover[index]}; fi
    count=$("$UMBRIEL_PIXEL_PROBE" "$image" count "$expected" "$region")
    if [[ $glyph == hover ]] && ((index != active)); then
      ((count == 0)) || { echo "button $index: glyph visible away from hover"; exit 1; }
    else
      ((count > 2)) || { echo "button $index: wrong glyph color with hover=$active backgrounds=$background glyph=$glyph"; exit 1; }
    fi
    other=${hover[index]}
    if ((index == active)); then other=${idle[index]}; fi
    count=$("$UMBRIEL_PIXEL_PROBE" "$image" count "$other" "$region")
    ((count == 0)) || { echo "button $index: stale glyph color after hover transition"; exit 1; }
  done
}

for background in true false; do
  for glyph in hover always; do
    sed -i "s/^button_background = .*/button_background = $background/; s/^button_glyph = .*/button_glyph = \"$glyph\"/" "$UMBRIEL_CONFIG"
    "$UMBRIEL" msg config-reload > /dev/null
    capture_at 0 0
    assert_glyphs -1
    for index in 0 1 2; do
      capture_at "$((264 + index * 28))" 80
      assert_glyphs "$index"
    done
    capture_at 0 0
    assert_glyphs -1
  done
done

# A pressed close control uses its hover glyph; leaving it cancels the visual press before release.
glyph=hover
sed -i 's/^button_glyph = .*/button_glyph = "hover"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
capture_at 320 80
pointer_hold "$output_w" "$output_h" press 272 -- move 220 80 mark cancelled hold release 272
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-1 "$image"
assert_glyphs 2
pointer_step cancelled
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-1 "$image"
assert_glyphs -1
pointer_release
capture_at 0 0
assert_glyphs -1

# The glyph's own alpha applies independently of the transparent background rectangle.
sed -i 's/^close_glyph = .*/close_glyph = "#00FF0080"/; s/^close_glyph_hover = .*/close_glyph_hover = "#00FF0080"/; s/^button_glyph = .*/button_glyph = "always"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
capture_at 0 0
read -r _ idle_green _ < <("$UMBRIEL_PIXEL_PROBE" "$image" max '25x25+387+87')
capture_at 320 80
read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$image" max '25x25+387+87')
((green > 100 && green < 210 && red == 0 && blue == 0 && green == idle_green)) || {
  echo "glyph alpha changed or was lost: idle green=$idle_green, hovered=$red $green $blue"
  exit 1
}
sed -i 's/^button_glyph = .*/button_glyph = "never"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
capture_at 320 80
count=$("$UMBRIEL_PIXEL_PROBE" "$image" count 'r > 0.1 || g > 0.1 || b > 0.1' '300x50+125+75')
((count == 0)) || { echo "disabled glyphs remained visible on hover"; exit 1; }
echo "per-button glyph colors, hover visibility, pressed feedback, alpha, and live reload work without backgrounds"
