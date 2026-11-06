#!/usr/bin/env bash
# Tall bars keep independently sized buttons; geometry changes apply on reload.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/title-bar-button-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/title-bar-button.png"
readonly FRAME_A="$UMBRIEL_RUNTIME_DIR/frame-a.png"
readonly FRAME_B="$UMBRIEL_RUNTIME_DIR/frame-b.png"

cat >> "$UMBRIEL_CONFIG" <<'TOML'

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[appearance.blur]
enabled = false

[appearance.title_bar]
mode = "forced"
height = 60
buttons = ["close"]
button_position = "right"
button_size = 12
button_gap = 8
button_margin = 13
button_corner_radius = 0
button_glyph = "never"

[colors.title_bar]
background = "#000000FF"
background_unfocused = "#000000FF"
close = "#FF0000FF"

[[window_rule]]
match.title = "^title-bar-button$"
opacity = 1.0
default_floating = true
default_floating_size_px = { width = 640, height = 480 }
default_position = { x = 100, y = 100, anchor = "top_left" }
TOML
"$UMBRIEL" msg config-reload > /dev/null
"$CLIENT" title-bar-button 640 480 > "$CLIENT_LOG" 2>&1 &
title_bar_wait_box title-bar-button 640x480+100+100

check_button() {
  local side=$1 window x y width button_x button_y
  # Move pointer out of the way to ensure no hover effects
  read -r output_w output_h < <("$UMBRIEL" outputs --json | jq -r '.[0].logical_size | "\(.width) \(.height)"')
  "$UMBRIEL_POINTER_CLIENT" "$output_w" "$output_h" move 0 0

  # Settle on two consecutive identical frames
  for ((i = 0; i < 20; i++)); do
    "$UMBRIEL" settle >/dev/null
    grim -o HEADLESS-1 "$FRAME_A"
    "$UMBRIEL" settle >/dev/null
    grim -o HEADLESS-1 "$FRAME_B"
    read -r largest differing < <("$UMBRIEL_PIXEL_PROBE" "$FRAME_A" diff "$FRAME_B")
    if (( differing == 0 )); then
      break
    fi
  done
  cp "$FRAME_B" "$SCREENSHOT"

  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "title-bar-button")')
  [[ -n $window ]] || { echo "title bar client missing"; exit 1; }
  x=$(jq -r '.x' <<< "$window")
  y=$(jq -r '.y' <<< "$window")
  width=$(jq -r '.w' <<< "$window")
  # Window client is placed at y=100 with height 60 title bar above it (y=40..100)
  # Button size is 12, vertically centered in 60px bar: verticalInset = (60 - 12)/2 = 24.
  # Button y bounds: (100 - 60 + 24) = 64..76, center is at y = 70.
  button_y=$((y - 60 + 24))
  button_x=$((x + 13))
  [[ $side == right ]] && button_x=$((x + width - 13 - 12))

  local offset red green blue
  # Test horizontal extents and neighboring background
  for offset in -1 0 5 11 12; do
    read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$SCREENSHOT" pixel "$((button_x + offset))" "$((button_y + 6))")
    if (( offset >= 0 && offset < 12 )); then
      # Idle button renders at alpha 0.85 (255 * 0.85 = 216.75 -> 217) over black background
      if (( red < 210 || red > 225 || green > 1 || blue > 1 )); then
        echo "$side button missing or wrong color at offset $offset: $red $green $blue"
        exit 1
      fi
    elif (( red > 1 || green > 1 || blue > 1 )); then
      echo "$side button exceeded configured size: $red $green $blue"
      exit 1
    fi
  done
}

check_button right
sed -i 's/^button_position = "right"$/button_position = "left"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
check_button left

echo "tall-bar button size, margin, mirroring, and live reload passed"
