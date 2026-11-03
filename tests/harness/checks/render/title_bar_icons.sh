#!/usr/bin/env bash
# Geometric button symbols and their backgrounds reload independently, scale with their window, and survive closing.
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
button_glyph = "always"
button_background = true
[colors.title_bar]
background = "#000000FF"
background_unfocused = "#000000FF"
text = "#000000FF"
text_unfocused = "#000000FF"
close = "#0000FFFF"
maximize = "#0000FFFF"
minimize = "#0000FFFF"
close_glyph = "#FFFFFFFF"
close_glyph_hover = "#FFFFFFFF"
maximize_glyph = "#FFFFFFFF"
maximize_glyph_hover = "#FFFFFFFF"
minimize_glyph = "#FFFFFFFF"
minimize_glyph_hover = "#FFFFFFFF"
[output.HEADLESS-1]
scale = 1
[[window_rule]]
match.title = "^ssd-icons$"
default_floating = true
default_floating_size_px = { width = 240, height = 140 }
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFF000000 "$UMBRIEL_UNMAP_CLIENT" ssd-icons 240 140 > "$UMBRIEL_RUNTIME_DIR/window.log" 2>&1 &
title_bar_wait_box ssd-icons 240x140+100+100
title_bar_wait_field ssd-icons minimizable true
image=$UMBRIEL_RUNTIME_DIR/title-bar-icons.png

capture() {
  "$UMBRIEL" settle
  grim -o HEADLESS-1 "$image"
}
flags() {
  sed -i "s/^button_glyph = .*/button_glyph = \"$1\"/; s/^button_background = .*/button_background = $2/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  capture
}
blue_pixels() { "$UMBRIEL_PIXEL_PROBE" "$image" count 'b > 0.4 && r < 0.3 && g < 0.3' "$region"; }
white_pixels() { "$UMBRIEL_PIXEL_PROBE" "$image" count 'r > 0.4 && g > 0.4' "$region"; }

for scale in 1 1.25 2; do
  sed -i "s/^scale = .*/scale = $scale/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  read -r output_w output_h < <("$UMBRIEL" outputs --json | jq -r '.[0].logical_size | "\(.width) \(.height)"')
  "$UMBRIEL_POINTER_CLIENT" "$output_w" "$output_h" move 0 0
  region=$(awk -v s="$scale" 'BEGIN { printf "%dx%d+%d+%d", 240*s, 40*s, 100*s, 60*s }')
  for side in left right; do
    sed -i "s/^button_position = .*/button_position = \"$side\"/" "$UMBRIEL_CONFIG"
    flags always true
    (( $(white_pixels) > 10 )) || { echo "scale=$scale side=$side: symbols missing on colored buttons"; exit 1; }
    # Each control must carry its own symbol, including the floating window's minimize control.
    for index in 0 1 2; do
      if [[ $side == right ]]; then
        x=$((310 - (2 - index) * 28))
      else
        x=$((110 + index * 28))
      fi
      button_region=$(awk -v s="$scale" -v x="$x" 'BEGIN { printf "%dx%d+%d+%d", 20*s, 20*s, x*s, 70*s }')
      count=$("$UMBRIEL_PIXEL_PROBE" "$image" count 'r > 0.4 && g > 0.4' "$button_region")
      (( count > 2 )) || { echo "scale=$scale side=$side: button $index lost its symbol"; exit 1; }
    done
    flags never true
    (( $(white_pixels) == 0 )) || { echo "symbols remained after disabling them"; exit 1; }
    background_area=$(blue_pixels)
    (( background_area > 100 )) || { echo "disabling symbols removed the button backgrounds"; exit 1; }
    flags always false
    symbol_area=$(white_pixels)
    (( $(blue_pixels) == 0 && symbol_area > 5 && symbol_area < background_area / 2 )) \
      || { echo "bare symbols did not retain their configured glyph colors"; exit 1; }
    flags never false
    (( $(blue_pixels) == 0 && $(white_pixels) == 0 )) || { echo "hidden controls still painted"; exit 1; }
  done
done

# Both visuals are off: the old close position is ordinary title space.
"$UMBRIEL_POINTER_CLIENT" "$output_w" "$output_h" move 320 80 press 272 release 272
"$UMBRIEL" settle
[[ -n $(title_bar_window ssd-icons) ]] || { echo "hidden buttons retained an invisible close target"; exit 1; }

# Symbols alone remain clickable and are copied into the window's close snapshot.
flags always false
sed -i '/^\[animation\]$/ { n; s/enabled = false/enabled = true/; }' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'
[animation.windows_out]
enabled = true
style = "fade"
duration_ms = 1000
curve = "linear"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" clock-freeze
"$UMBRIEL_POINTER_CLIENT" "$output_w" "$output_h" move 320 80 press 272 release 272
title_bar_wait_absent ssd-icons
grim -o HEADLESS-1 "$image"
(( $(white_pixels) > 5 )) || { echo "close snapshot lost its button symbols"; exit 1; }
for x in 254 282 310; do
  button_region="40x40+$((x * 2))+140"
  count=$("$UMBRIEL_PIXEL_PROBE" "$image" count 'r > 0.4 && g > 0.4' "$button_region")
  (( count > 2 )) || { echo "close snapshot lost the cached symbol at x=$x"; exit 1; }
done
"$UMBRIEL" clock-advance 1100
grim -o HEADLESS-1 "$image"
(( $(white_pixels) == 0 )) || { echo "button symbols survived their close snapshot"; exit 1; }
echo "button symbols and backgrounds reload independently at fractional scale and close with their window"
