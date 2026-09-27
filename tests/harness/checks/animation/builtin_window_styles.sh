#!/usr/bin/env bash
# Built-in slide keeps more opacity than fade at the same point in the timeline, so its existing movement remains
# visible during both window opening and closing. Built-in popin shrinks a closing snapshot toward its own centre.
# Built-in fade applies one alpha to the whole window: a red parent fully covered by a blue subsurface never shows
# through it while opening or closing, as it would if each buffer faded on its own.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SUBSURFACE_CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/builtin-window-styles.png"
readonly DURATION_MS=5000

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[animation]
duration_ms = 5000
curve = "linear"

[animation.windows_in]
style = "fade" # OPEN_STYLE

[animation.windows_out]
style = "fade" # CLOSE_STYLE

[animation.windows_move]
enabled = false

[animation.dim_unfocused]
enabled = false

[[window_rule]]
match.title = "^style-fade$"
default_floating = true
default_floating_size_px = { width = 400, height = 240 }
default_position = { x = 100, y = 180, anchor = "top_left" }

[[window_rule]]
match.title = "^style-slide$"
default_floating = true
default_floating_size_px = { width = 400, height = 240 }
default_position = { x = 760, y = 180, anchor = "top_left" }

[[window_rule]]
match.title = "^style-group$"
default_floating = true
default_floating_size_px = { width = 400, height = 240 }
default_position = { x = 760, y = 440, anchor = "top_left" }

[[window_rule]]
match.title = "^style-popin$"
default_floating = true
default_floating_size_px = { width = 400, height = 240 }
default_position = { x = 100, y = 440, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_window() {
  local title=$1
  for _ in $(seq 100); do
    if "$UMBRIEL" windows --json | jq -e --arg title "$title" '.[] | select(.title == $title)' > /dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "timed out waiting for $title"
  return 1
}

spawn() {
  local title=$1
  FILL_COLOR=0xFF0000FF "$CLIENT" "$title" 400 240 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  wait_for_window "$title"
}

window_id() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'
}

sample_rgb() {
  local x=$1 y=$2
  grim "$IMAGE"
  magick "$IMAGE" -alpha off -crop "40x40+$x+$y" +repage \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.b)]\n' info:
}

assert_group_fade() {
  local phase=$1 red=$2 blue=$3
  if ! ((blue >= 90 && blue <= 165)); then
    echo "$phase subsurface window was outside the middle of its fade: blue=$blue"
    exit 1
  fi
  if ((red > 15)); then
    echo "$phase covered parent showed through its subsurface: red=$red blue=$blue"
    exit 1
  fi
}

sample_blue() {
  local x=$1 y=$2
  grim "$IMAGE"
  magick "$IMAGE" -crop "40x40+$x+$y" -format '%[fx:round(255*mean.b)]\n' info:
}

blue_bounds() {
  grim "$IMAGE"
  "$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.3 && r < 0.2 && g < 0.2'
}

assert_slide_brighter() {
  local phase=$1 fade=$2 slide=$3
  if ! (( fade >= 90 && fade <= 165 )); then
    echo "$phase fade sample was outside the middle of its timeline: $fade"
    exit 1
  fi
  if ! (( slide >= 150 && slide <= 215 && slide >= fade + 30 )); then
    echo "$phase slide was not visibly more opaque than fade: fade=$fade slide=$slide"
    exit 1
  fi
}

# Animation time only moves by clock-advance: samples land at 2350 ms (0.47 of each 5000 ms timeline), and advancing
# 5000 ms finishes every running animation.
"$UMBRIEL" clock-freeze
spawn style-fade
"$UMBRIEL" clock-advance 2350
fade_open=$(sample_blue 280 280)
"$UMBRIEL" clock-advance 5000

sed -i 's/^style = "fade" # OPEN_STYLE$/style = "slide" # OPEN_STYLE/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
spawn style-slide
"$UMBRIEL" clock-advance 2350
slide_open=$(sample_blue 940 310)
assert_slide_brighter opening "$fade_open" "$slide_open"
"$UMBRIEL" clock-advance 5000

"$UMBRIEL" msg "window-close:$(window_id style-fade)" > /dev/null
"$UMBRIEL" clock-advance 2350
fade_close=$(sample_blue 280 280)
"$UMBRIEL" clock-advance 5000

sed -i 's/^style = "fade" # CLOSE_STYLE$/style = "slide" # CLOSE_STYLE/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg "window-close:$(window_id style-slide)" > /dev/null
"$UMBRIEL" clock-advance 2350
slide_close=$(sample_blue 940 310)
assert_slide_brighter closing "$fade_close" "$slide_close"
"$UMBRIEL" clock-advance 5000

sed -i 's/^style = "slide" # CLOSE_STYLE$/style = "popin" # CLOSE_STYLE/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
spawn style-popin
"$UMBRIEL" clock-advance 5000
read -r popin_x popin_y popin_w popin_h <<< "$(blue_bounds)"
if ((popin_x < 98 || popin_x > 102 || popin_y < 438 || popin_y > 442 \
    || popin_w < 398 || popin_w > 402 || popin_h < 238 || popin_h > 242)); then
  echo "popin window did not settle at its configured box: $popin_x $popin_y $popin_w $popin_h"
  exit 1
fi
"$UMBRIEL" msg "window-close:$(window_id style-popin)" > /dev/null
"$UMBRIEL" clock-advance 2350
read -r mid_x mid_y mid_w mid_h <<< "$(blue_bounds)"
# Linear 5000 ms close sampled near 0.47: scale is about 0.906 of the captured 400x240 box, centred on 300, 560.
if ((mid_x < 108 || mid_x + mid_w > 492 || mid_y < 444 || mid_y + mid_h > 676)); then
  echo "popin close snapshot did not shrink inside its captured box: $mid_x $mid_y $mid_w $mid_h"
  exit 1
fi
mid_cx=$((2 * mid_x + mid_w))
mid_cy=$((2 * mid_y + mid_h))
if ((mid_cx < 594 || mid_cx > 606 || mid_cy < 1114 || mid_cy > 1126)); then
  echo "popin close snapshot did not shrink toward its centre: centre=$((mid_cx / 2)) $((mid_cy / 2))"
  exit 1
fi

"$UMBRIEL" clock-advance 5000

sed -i 's/^style = "slide" # OPEN_STYLE$/style = "fade" # OPEN_STYLE/' "$UMBRIEL_CONFIG"
sed -i 's/^style = "popin" # CLOSE_STYLE$/style = "fade" # CLOSE_STYLE/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$SUBSURFACE_CLIENT" style-group 400 240 > "$UMBRIEL_RUNTIME_DIR/style-group.log" 2>&1 &
group_pid=$!
wait_for_window style-group
"$UMBRIEL" clock-advance 2350
read -r group_open_red group_open_blue <<< "$(sample_rgb 940 540)"
assert_group_fade opening "$group_open_red" "$group_open_blue"
"$UMBRIEL" clock-advance 5000
kill "$group_pid"
"$UMBRIEL" clock-advance 2350
read -r group_close_red group_close_blue <<< "$(sample_rgb 940 540)"
assert_group_fade closing "$group_close_red" "$group_close_blue"

echo "built-in slide stayed distinct from fade, popin shrank the close snapshot, and fade kept subsurfaces opaque over their parent"
