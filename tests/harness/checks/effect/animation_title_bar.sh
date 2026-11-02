#!/usr/bin/env bash
# SSD and client pixels share one animation capture, UV domain, and lifecycle alpha.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/animation-title-bar.png"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/client.log"

cat > "$UMBRIEL_RUNTIME_DIR/title-bar-frame.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
    vec4 source = umbriel_sample(uv);
    float marker = step(0.5, umbriel_progress) * (1.0 - step(0.5, uv.y));
    return vec4(source.r, marker * source.a, source.b, source.a);
}
GLSL
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[colors]
backdrop = "#000000FF"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "forced"
height = 28
[colors.title_bar]
background = "#FF0000FF"
background_unfocused = "#FF0000FF"
text = "#FF0000FF"
text_unfocused = "#FF0000FF"
close = "#FF0000FF"
maximize = "#FF0000FF"
[animation]
duration_ms = 1000
curve = "linear"
[effects.preset.title_bar_frame]
kind = "animation"
shader = "title-bar-frame.glsl"
[animation.windows_in]
style = "fade"
effect = "title_bar_frame"
[animation.windows_move]
effect = "title_bar_frame"
[animation.windows_out]
style = "fade"
effect = "title_bar_frame"
[animation.dim_unfocused]
enabled = false
[[window_rule]]
match.title = "^animation-title-bar-"
default_floating = true
default_floating_size_px = { width = 400, height = 240 }
default_position = { x = 200, y = 200, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

window() {
  "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'
}

spawn() {
  local title=$1 entry=
  FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" "$title" 400 240 > "$CLIENT_LOG" 2>&1 &
  for _ in $(seq 100); do
    entry=$(window "$title")
    if jq -e '.title_bar and .x == 200 and .y == 200 and .w == 400 and .h == 240' \
        <<< "$entry" > /dev/null; then
      window_id=$(jq -r '.id' <<< "$entry")
      return 0
    fi
    sleep 0.025
  done
  echo "$title did not map with its server-side title bar: $entry"
  return 1
}

close_window() {
  local title=$1
  "$UMBRIEL" msg window-close "$window_id" > /dev/null
  for _ in $(seq 100); do
    if [[ -z $(window "$title") ]] && rg -q '^unmapped$' "$CLIENT_LOG"; then
      return 0
    fi
    sleep 0.025
  done
  echo "$title did not unmap"
  return 1
}

assert_pixel() {
  local label=$1 x=$2 y=$3 expected_r=$4 expected_g=$5 expected_b=$6 r g b
  read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
  if (( r < expected_r - 12 || r > expected_r + 12
      || g < expected_g - 12 || g > expected_g + 12
      || b < expected_b - 12 || b > expected_b + 12 )); then
    echo "$label: pixel $x,$y was $r $g $b, expected $expected_r $expected_g $expected_b"
    return 1
  fi
}

assert_frame() {
  local label=$1 x=$2 y=$3 green=$4
  grim -o HEADLESS-1 "$IMAGE"
  # This pixel is low in the title bar, but high in the combined frame. Separately binding the shader to the bar
  # would give it a different UV and leave its green channel off after the midpoint.
  assert_pixel "$label title bar" "$((x + 30))" "$((y - 6))" 255 "$green" 0
  assert_pixel "$label upper client" "$((x + 30))" "$((y + 40))" 0 "$green" 255
  assert_pixel "$label lower client" "$((x + 30))" "$((y + 200))" 0 0 255
}

assert_fade() {
  local label=$1 minimum=$2 maximum=$3 bar_r bar_g bar_b body_r body_g body_b
  grim -o HEADLESS-1 "$IMAGE"
  read -r bar_r bar_g bar_b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 230 194)
  read -r body_r body_g body_b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 230 240)
  if (( bar_r < minimum || bar_r > maximum || body_b < minimum || body_b > maximum
      || bar_r < body_b - 12 || bar_r > body_b + 12
      || bar_g > 12 || bar_b > 12 || body_r > 12 || body_g > 12 )); then
    echo "$label: title bar and client did not share one fade: bar=$bar_r $bar_g $bar_b, client=$body_r $body_g $body_b"
    return 1
  fi
}

# Animation time only moves by clock-advance. Each transition is sampled at 250 and 750 ms, then finished at 1000 ms.
"$UMBRIEL" clock-freeze > /dev/null
spawn animation-title-bar-shader
"$UMBRIEL" clock-advance 250 > /dev/null
assert_frame opening-before-marker 200 200 0
"$UMBRIEL" clock-advance 500 > /dev/null
assert_frame opening-after-marker 200 200 255
"$UMBRIEL" clock-advance 250 > /dev/null
"$UMBRIEL" settle > /dev/null
assert_frame opening-finished 200 200 0

"$UMBRIEL" msg window-move-by 200 80 --window "$window_id" > /dev/null
"$UMBRIEL" clock-advance 250 > /dev/null
assert_frame moving-before-marker 250 220 0
"$UMBRIEL" clock-advance 500 > /dev/null
assert_frame moving-after-marker 350 260 255
"$UMBRIEL" clock-advance 250 > /dev/null
"$UMBRIEL" settle > /dev/null
assert_frame moving-finished 400 280 0

close_window animation-title-bar-shader
"$UMBRIEL" clock-advance 250 > /dev/null
assert_frame closing-before-marker 400 280 0
"$UMBRIEL" clock-advance 500 > /dev/null
assert_frame closing-after-marker 400 280 255
"$UMBRIEL" clock-advance 250 > /dev/null
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-1 "$IMAGE"
remaining=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count '(r > 0.1 && b < 0.05) || (b > 0.1 && r < 0.05)')
if ((remaining > 0)); then
  echo "closing left title bar or client pixels behind: $remaining"
  exit 1
fi

# The built-in group fade must also avoid fading the title bar once before capture and again during composition.
sed -i '/^effect = "title_bar_frame"$/d' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
spawn animation-title-bar-fade
"$UMBRIEL" clock-advance 250 > /dev/null
assert_fade opening-quarter 35 95
"$UMBRIEL" clock-advance 500 > /dev/null
assert_fade opening-three-quarters 160 220
"$UMBRIEL" clock-advance 250 > /dev/null
"$UMBRIEL" settle > /dev/null
close_window animation-title-bar-fade
"$UMBRIEL" clock-advance 250 > /dev/null
assert_fade closing-quarter 160 220
"$UMBRIEL" clock-advance 500 > /dev/null
assert_fade closing-three-quarters 35 95
"$UMBRIEL" clock-advance 250 > /dev/null
"$UMBRIEL" settle > /dev/null

echo "title bar and client shared opening, movement, closing shader bounds, and built-in fade alpha"
