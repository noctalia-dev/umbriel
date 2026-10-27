#!/usr/bin/env bash
# The SSD title bar blurs its own backdrop using the owning window's rules, even over opaque client content.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"

cat >>"$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 16
drag_opacity = 1.0
[overview]
background_blur = false
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "forced"
height = 28
[colors.title_bar]
background = "#00000040"
background_unfocused = "#00000040"
[appearance.blur]
enabled = true
optimized = true
passes = 2
radius = 6
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0
[[window_rule]]
match.title = "^ssd-blur$"
blur = true
blur_optimized = false
blur_ignore_alpha = 0.0
default_floating = true
default_floating_size_px = { width = 480, height = 300 }
default_position = { x = 100, y = 128, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload >/dev/null
mkfifo "$UMBRIEL_RUNTIME_DIR/wallpaper.fifo"
exec {wallpaper_fd}<>"$UMBRIEL_RUNTIME_DIR/wallpaper.fifo"
LAYER_CONTROL=1 "$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 <"$UMBRIEL_RUNTIME_DIR/wallpaper.fifo" \
  >"$UMBRIEL_RUNTIME_DIR/wallpaper.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/wallpaper.log" ready 1
echo 'color magenta FFFF00FF' >&"$wallpaper_fd"
await_lines "$UMBRIEL_RUNTIME_DIR/wallpaper.log" 'done magenta' 1
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 bottom-layer >"$UMBRIEL_RUNTIME_DIR/square.log" 2>&1 &
square_pid=$!
await_lines "$UMBRIEL_RUNTIME_DIR/square.log" ready 1
mkfifo "$UMBRIEL_RUNTIME_DIR/window.fifo"
exec {window_fd}<>"$UMBRIEL_RUNTIME_DIR/window.fifo"
REPAINT_ON_STDIN=1 FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" ssd-blur 480 300 \
  <"$UMBRIEL_RUNTIME_DIR/window.fifo" >"$UMBRIEL_RUNTIME_DIR/window.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/window.log" mapped 1
title_bar_wait_box ssd-blur 480x300+100+128
title_bar_wait_field ssd-blur title_bar true

image=$UMBRIEL_RUNTIME_DIR/title-bar-blur.png
sample() {
  "$UMBRIEL" settle >/dev/null
  grim -o HEADLESS-1 "$image"
  "$UMBRIEL_PIXEL_PROBE" "$image" pixel "$1" "$2"
}
reload() { "$UMBRIEL" msg config-reload >/dev/null; }
expect_blur() {
  local label=$1 green
  read -r _ green _ < <(sample "$sample_x" "$sample_y")
  if ((green < 40 || green > 160)); then
    echo "$label: title bar did not blur the square's edge: green=$green"
    exit 1
  fi
}
expect_sharp() {
  local label=$1 green
  read -r _ green _ < <(sample "$sample_x" "$sample_y")
  if ((green > 4)); then
    echo "$label: title bar retained live blur: green=$green"
    exit 1
  fi
}

# Probe the green square's vertical edge beneath the bar, away from text and buttons.
# Its client content is opaque, so neither client alpha nor its buffer can supply this blur mask.
for scale in 1 1.25; do
  if [[ $scale == 1 ]]; then
    sample_x=202 sample_y=112
    corner_x=100 corner_y=100
    seam_y=127 outside_y=99
  else
    cat >>"$UMBRIEL_CONFIG" <<'EOF'
[output."HEADLESS-1"]
scale = 1.25
EOF
    reload
    sample_x=253 sample_y=140
    corner_x=125 corner_y=125
    seam_y=158 outside_y=123
  fi
  expect_blur "scale $scale"
  read -r _ green _ < <(sample "$corner_x" "$corner_y")
  ((green > 245)) || {
    echo "blur or background escaped the rounded top corner"
    exit 1
  }
  read -r _ green _ < <(sample "$corner_x" "$seam_y")
  ((green > 120 && green < 220)) || {
    echo "title bar rounded its bottom seam: green=$green"
    exit 1
  }
  read -r _ green _ < <(sample "$sample_x" "$outside_y")
  ((green < 5)) || {
    echo "title bar blur escaped above the frame"
    exit 1
  }

  sed -i 's/^blur = true$/blur = false/' "$UMBRIEL_CONFIG"
  reload
  expect_sharp "window blur disabled"
  sed -i 's/^blur = false$/blur = true/' "$UMBRIEL_CONFIG"
  reload
  expect_blur "window blur re-enabled"

  sed -i 's/^blur_ignore_alpha = 0.0$/blur_ignore_alpha = 0.5/' "$UMBRIEL_CONFIG"
  reload
  expect_sharp "title bar alpha below window threshold"
  sed -i 's/^blur_ignore_alpha = 0.5$/blur_ignore_alpha = 0.0/' "$UMBRIEL_CONFIG"
  reload
  expect_blur "alpha threshold removed"

  sed -i '/^\[appearance.blur\]$/,/^\[/s/^enabled = true$/enabled = false/' "$UMBRIEL_CONFIG"
  reload
  expect_sharp "global blur disabled"
  sed -i '/^\[appearance.blur\]$/,/^\[/s/^enabled = false$/enabled = true/' "$UMBRIEL_CONFIG"
  reload
  expect_blur "global blur re-enabled"

  # The optimized cache contains the magenta wallpaper, excluding the green bottom-layer square.
  sed -i 's/^blur_optimized = false$/blur_optimized = true/' "$UMBRIEL_CONFIG"
  reload
  expect_sharp "window optimized blur override"
  if [[ $scale == 1 ]]; then
    # A drag must temporarily use the live scene, then return to the window's optimized setting on release.
    for background in translucent opaque; do
      if [[ $background == opaque ]]; then
        sed -i 's/#00000040/#000000FF/g; s/^drag_opacity = 1.0$/drag_opacity = 0.5/' "$UMBRIEL_CONFIG"
        reload
      fi
      pointer_hold 1280 720 move 400 114 press 272 move 420 114 -- release 272
      expect_blur "dragging an optimized window with $background background"
      pointer_release
      expect_sharp "optimized blur restored after drag"
      pointer_hold 1280 720 move 420 114 press 272 move 400 114 -- release 272
      pointer_release
      title_bar_wait_box ssd-blur 480x300+100+128
    done
    sed -i 's/#000000FF/#00000040/g; s/^drag_opacity = 0.5$/drag_opacity = 1.0/' "$UMBRIEL_CONFIG"
    reload
  fi
  sed -i 's/^blur_optimized = true$/blur_optimized = false/' "$UMBRIEL_CONFIG"
  reload
  expect_blur "window live blur override"
done

# Repainting this window must not tint its own title bar blur across the content seam.
before=$(sample 500 158)
echo R >&"$window_fd"
await_lines "$UMBRIEL_RUNTIME_DIR/window.log" repainted 1
after=$(sample 500 158)
[[ $before == "$after" ]] || {
  echo "title bar blurred its own client content: $before changed to $after"
  exit 1
}
echo B >&"$window_fd"
await_lines "$UMBRIEL_RUNTIME_DIR/window.log" repainted 2

# The overview copies the window's title bar and its blur policy at the card's zoom.
"$UMBRIEL" msg overview-open >/dev/null
"$UMBRIEL" settle >/dev/null
grim -o HEADLESS-1 "$image"
read -r card_x card_y card_w card_h < <(
  "$UMBRIEL_PIXEL_PROBE" "$image" bbox 'r < 0.02 && g < 0.02 && b > 0.98'
)
((card_w > 0 && card_h > 0)) || {
  echo "overview did not show the opaque client"
  exit 1
}
preview_x=$((card_x + 102 * card_w / 480))
preview_y=$((card_y - 14 * card_w / 480))
read -r _ preview_green _ < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel "$preview_x" "$preview_y")
sed -i 's/^blur = true$/blur = false/' "$UMBRIEL_CONFIG"
reload
"$UMBRIEL" msg overview-open >/dev/null
read -r _ sharp_green _ < <(sample "$preview_x" "$preview_y")
((preview_green > sharp_green + 25)) || {
  echo "overview title bar did not follow window blur: enabled=$preview_green disabled=$sharp_green"
  exit 1
}
sed -i 's/^blur = false$/blur = true/' "$UMBRIEL_CONFIG"
reload
"$UMBRIEL" msg overview-open >/dev/null
read -r _ restored_green _ < <(sample "$preview_x" "$preview_y")
((restored_green > sharp_green + 25)) || {
  echo "overview title bar did not restore blur"
  exit 1
}
"$UMBRIEL" msg overview-close >/dev/null
"$UMBRIEL" settle >/dev/null

# A close snapshot must keep the blur on its first frame and fade it with the rest of the window.
sed -i '/^\[animation\]$/,/^\[/s/^enabled = false$/enabled = true/' "$UMBRIEL_CONFIG"
cat >>"$UMBRIEL_CONFIG" <<'EOF'
[animation.windows_out]
enabled = true
style = "fade"
duration_ms = 1000
curve = "linear"
[animation.windows_in]
enabled = false
EOF
for style in fade slide; do
  sed -i "s/^style = .*/style = \"$style\"/" "$UMBRIEL_CONFIG"
  reload
  if [[ $style == slide ]]; then
    FILL_COLOR=0xFF5577AA "$UMBRIEL_UNMAP_CLIENT" ssd-blur 480 300 >"$UMBRIEL_RUNTIME_DIR/slide.log" 2>&1 &
    await_lines "$UMBRIEL_RUNTIME_DIR/slide.log" mapped 1
    title_bar_wait_box ssd-blur 480x300+100+128
  fi
  "$UMBRIEL" settle >/dev/null
  "$UMBRIEL" clock-freeze >/dev/null
  id=$(title_bar_window ssd-blur | jq -r '.id')
  "$UMBRIEL" msg "window-close:$id" >/dev/null
  title_bar_wait_absent ssd-blur
  grim -o HEADLESS-1 "$image"
  read -r _ first_green _ < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel "$sample_x" "$sample_y")
  ((first_green > 40)) || {
    echo "$style close snapshot dropped the title bar blur"
    exit 1
  }
  "$UMBRIEL" clock-advance 500 >/dev/null
  grim -o HEADLESS-1 "$image"
  # Slide moves the title bar 40 logical pixels halfway through its 80-pixel exit.
  half_y=$sample_y
  [[ $style == slide ]] && half_y=$((sample_y + 50))
  read -r _ half_green _ < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel "$sample_x" "$half_y")
  ((half_green > 10 && half_green < first_green - 10)) || {
    echo "$style close snapshot did not fade title bar blur: first=$first_green half=$half_green"
    exit 1
  }
  "$UMBRIEL" clock-advance 500 >/dev/null
  grim -o HEADLESS-1 "$image"
  read -r _ green _ < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel "$sample_x" "$half_y")
  ((green < 5)) || {
    echo "title bar blur survived the closed window"
    exit 1
  }
  "$UMBRIEL" clock-resume >/dev/null
done

# Transparent content must meet the title bar with square blur corners on both
# sides. Move a patterned wallpaper edge beneath each corner so missing blur
# reveals magenta instead of the green carried across that edge by the kernel.
kill "$square_pid"
wait "$square_pid" || true
sed -i '/^\[animation\]$/,/^\[/s/^enabled = true$/enabled = false/; s/^blur_optimized = false$/blur_optimized = true/' "$UMBRIEL_CONFIG"
reload
FILL_COLOR=0x66000000 "$UMBRIEL_UNMAP_CLIENT" ssd-blur 480 300 >"$UMBRIEL_RUNTIME_DIR/transparent.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/transparent.log" mapped 1
title_bar_wait_box ssd-blur 480x300+100+128
echo 'child-color corner-pattern FF00FF00' >&"$wallpaper_fd"
await_lines "$UMBRIEL_RUNTIME_DIR/wallpaper.log" 'done corner-pattern' 1

expect_content_corner() {
  local label=$1 rounded=$2 x=$3 y=$4 green
  read -r _ green _ < <(sample "$x" "$y")
  if ((rounded)); then
    ((green < 5)) || {
      echo "$label: blur escaped the rounded corner: green=$green"
      exit 1
    }
  else
    ((green > 15 && green < 150)) || {
      echo "$label: transparent content retained a rounded blur corner: green=$green"
      exit 1
    }
  fi
}

for scale in 1 1.25; do
  sed -i "s/^scale = .*$/scale = $scale/" "$UMBRIEL_CONFIG"
  reload
  for side in left right; do
    if [[ $side == left ]]; then
      pattern_x=106 corner_x=101
    else
      pattern_x=374 corner_x=578
    fi
    echo "child-move $side-$scale $pattern_x 28" >&"$wallpaper_fd"
    await_lines "$UMBRIEL_RUNTIME_DIR/wallpaper.log" "done $side-$scale" 1
    pixel_x=$(awk -v x="$corner_x" -v scale="$scale" 'BEGIN { printf "%d", x * scale }')
    pixel_y=$(awk -v scale="$scale" 'BEGIN { printf "%d", 129 * scale }')
    expect_content_corner "$side seam at scale $scale" 0 "$pixel_x" "$pixel_y"

    # The title bar retains its outer top arc. Hiding it restores rounding to
    # the content, and showing it again must clear that rounding immediately.
    outer_y=$(awk -v scale="$scale" 'BEGIN { printf "%d", 101 * scale }')
    expect_content_corner "$side outer frame at scale $scale" 1 "$pixel_x" "$outer_y"
    sed -i 's/^mode = "forced"$/mode = "off"/' "$UMBRIEL_CONFIG"
    reload
    title_bar_wait_field ssd-blur title_bar false
    expect_content_corner "$side without a title bar at scale $scale" 1 "$pixel_x" "$pixel_y"
    sed -i 's/^mode = "off"$/mode = "forced"/' "$UMBRIEL_CONFIG"
    reload
    title_bar_wait_field ssd-blur title_bar true
    expect_content_corner "$side restored title bar at scale $scale" 0 "$pixel_x" "$pixel_y"

    echo "child-move bottom-$side-$scale $pattern_x 328" >&"$wallpaper_fd"
    await_lines "$UMBRIEL_RUNTIME_DIR/wallpaper.log" "done bottom-$side-$scale" 1
    bottom_y=$(awk -v scale="$scale" 'BEGIN { printf "%d", 426 * scale }')
    expect_content_corner "$side bottom frame at scale $scale" 1 "$pixel_x" "$bottom_y"
  done
done

# Locate the card from an opaque blue title bar, so the probes follow its real
# zoom and placement instead of assuming the overview's workspace dimensions.
# A visible border keeps the card decorated, including its nonzero radius.
sed -i 's/#00000040/#0000FFFF/g; s/^border_width = 0$/border_width = 2/' "$UMBRIEL_CONFIG"
reload
for side in left right; do
  if [[ $side == left ]]; then
    pattern_x=106
  else
    pattern_x=374
  fi
  echo "child-move overview-$side $pattern_x 28" >&"$wallpaper_fd"
  await_lines "$UMBRIEL_RUNTIME_DIR/wallpaper.log" "done overview-$side" 1
  "$UMBRIEL" msg overview-open >/dev/null
  "$UMBRIEL" settle >/dev/null
  grim -o HEADLESS-1 "$image"
  read -r bar_x bar_y bar_w bar_h < <(
    "$UMBRIEL_PIXEL_PROBE" "$image" bbox 'r < 0.02 && g < 0.02 && b > 0.98'
  )
  ((bar_w > 0 && bar_h > 0)) || {
    echo "transparent preview lost its title bar"
    exit 1
  }
  if [[ $side == left ]]; then
    preview_x=$((bar_x + 1))
  else
    preview_x=$((bar_x + bar_w - 2))
  fi
  preview_y=$((bar_y + bar_h + 1))
  expect_content_corner "$side overview seam" 0 "$preview_x" "$preview_y"
  "$UMBRIEL" msg overview-close >/dev/null
done
echo "title bar blur follows window rules, scaling, dragging, overview, close animations, and transparent content corners"
