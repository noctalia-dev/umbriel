#!/usr/bin/env bash
# Button radii inherit the resolved window radius and preserve their shape through reloads, scaling, and previews.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 10
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "forced"
height = 28
button_position = "right"
button_corner_radius = -1
button_glyph = "never"
[colors.title_bar]
background = "#FF00FFFF"
background_unfocused = "#FF00FFFF"
close = "#00FF00FF"
maximize = "#00FF00FF"
[overview]
background_blur = false
[output.HEADLESS-1]
scale = 1
[[window_rule]]
match.title = "^ssd-button-radius$"
corner_radius = 3
default_floating = true
default_floating_size_px = { width = 240, height = 140 }
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL_UNMAP_CLIENT" ssd-button-radius 240 140 >"$UMBRIEL_RUNTIME_DIR/window.log" 2>&1 &
title_bar_wait_box ssd-button-radius 240x140+100+100
image=$UMBRIEL_RUNTIME_DIR/button-radius.png

area() {
  "$UMBRIEL" settle >/dev/null
  grim -o HEADLESS-1 "$image"
  "$UMBRIEL_PIXEL_PROBE" "$image" count 'g > 0.8 && r < 0.2 && b < 0.2'
}
radius() {
  sed -i "s/^button_corner_radius = .*/button_corner_radius = $1/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload >/dev/null
  area
}
overview_area() {
  radius "$1" >/dev/null
  "$UMBRIEL" msg overview-open >/dev/null
  area
  "$UMBRIEL" msg overview-close >/dev/null
}

for scale in 1 1.25 2; do
  sed -i "s/^scale = .*/scale = $scale/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload >/dev/null
  read -r output_w output_h < <("$UMBRIEL" outputs --json | jq -r '.[0].logical_size | "\(.width) \(.height)"')
  "$UMBRIEL_POINTER_CLIENT" "$output_w" "$output_h" move 0 0
  for height in 28 40; do
    sed -i "s/^height = .*/height = $height/" "$UMBRIEL_CONFIG"
    for side in left right; do
      sed -i "s/^button_position = .*/button_position = \"$side\"/" "$UMBRIEL_CONFIG"
      square=$(radius 0)
      rounded=$(radius 3)
      circle=$(radius 100)
      inherited=$(radius -1)
      ((square > rounded && rounded > circle)) || {
        echo "scale=$scale height=$height side=$side: shapes did not change: $square $rounded $circle"
        exit 1
      }
      [[ $inherited == "$rounded" ]] || {
        echo "buttons did not inherit the window's radius override"
        exit 1
      }
      if [[ $scale != 1.25 ]]; then
        diameter=$(((height + 1) / 2))
        ((square == 2 * diameter * diameter * scale * scale)) || {
          echo "square buttons lost corner pixels"
          exit 1
        }
      fi
      sed -i 's/^corner_radius = 3$/corner_radius = 0/' "$UMBRIEL_CONFIG"
      "$UMBRIEL" msg config-reload >/dev/null
      [[ $(area) == "$square" ]] || {
        echo "square window radius did not reach the buttons"
        exit 1
      }
      sed -i 's/^corner_radius = 0$/corner_radius = 100/' "$UMBRIEL_CONFIG"
      "$UMBRIEL" msg config-reload >/dev/null
      [[ $(area) == "$circle" ]] || {
        echo "large inherited button radius was not capped to a circle"
        exit 1
      }
      sed -i 's/^corner_radius = 100$/corner_radius = 3/' "$UMBRIEL_CONFIG"
      "$UMBRIEL" msg config-reload >/dev/null
      title_bar_wait_box ssd-button-radius 240x140+100+100
    done
  done
done

# The preview receives the same per-window radius as the live title bar.
preview_square=$(overview_area 0)
preview_rounded=$(overview_area 3)
preview_circle=$(overview_area 100)
preview_inherited=$(overview_area -1)
((preview_square > preview_rounded && preview_rounded > preview_circle)) || {
  echo "overview lost button shapes"
  exit 1
}
[[ $preview_inherited == "$preview_rounded" ]] || {
  echo "overview did not inherit the window's button radius"
  exit 1
}

# A close snapshot keeps the rounded square rather than reverting to circular buttons.
rounded=$(radius 3)
sed -i '/^\[animation\]$/,/^\[/s/^enabled = false$/enabled = true/' "$UMBRIEL_CONFIG"
cat >>"$UMBRIEL_CONFIG" <<'EOF'
[animation.windows_out]
enabled = true
style = "fade"
duration_ms = 1000
curve = "linear"
EOF
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" settle >/dev/null
"$UMBRIEL" clock-freeze >/dev/null
id=$(title_bar_window ssd-button-radius | jq -r '.id')
"$UMBRIEL" msg window-close "$id" >/dev/null
title_bar_wait_absent ssd-button-radius
grim -o HEADLESS-1 "$image"
snapshot=$("$UMBRIEL_PIXEL_PROBE" "$image" count 'g > 0.8 && r < 0.2 && b < 0.2')
[[ $snapshot == "$rounded" ]] || {
  echo "close snapshot changed button corners: $rounded to $snapshot"
  exit 1
}
"$UMBRIEL" clock-advance 1000 >/dev/null
"$UMBRIEL" clock-resume >/dev/null
echo "button radii inherit window corners, reload live, scale, and survive overview and close snapshots"
