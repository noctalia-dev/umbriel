#!/usr/bin/env bash
# Bright green makes detached shadows observable against both black wallpaper
# and the blue client. Check intermediate silhouette edges, not just teardown.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/shadows.png"
cat > "$UMBRIEL_RUNTIME_DIR/half.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return uv.x < 0.5 ? umbriel_sample(uv) : vec4(0.0); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
shadow = "#00FF00FF"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = true
softness = 24
offset_x = 0
offset_y = 0
[animation]
duration_ms = 2000
curve = "linear"
[animation.windows_move]
enabled = false
[animation.windows_in]
shader = "half.glsl"
[animation.windows_out]
shader = "half.glsl"
[[window_rule]]
match.title = "^shadow-caster$"
default_floating = true
default_position = { x = 200, y = 120, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
# Animation time only moves by clock-advance: samples land 200 ms into each 2000 ms timeline.
"$UMBRIEL" clock-freeze
"$UMBRIEL_UNMAP_CLIENT" shadow-caster 700 400 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "shadow-caster")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
"$UMBRIEL" clock-advance 200
window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "shadow-caster")')
id=$(jq -r .id <<< "$window")
x=$(jq -r .x <<< "$window")
y=$(jq -r .y <<< "$window")
w=$(jq -r .w <<< "$window")
h=$(jq -r .h <<< "$window")
pixel() {
  read -r r g b < <(magick "$IMAGE" -crop "2x2+$1+$2" \
    -format '%[fx:round(mean.r*255)] %[fx:round(mean.g*255)] %[fx:round(mean.b*255)]\n' info:)
}
assert_half_shadow() {
  grim "$IMAGE"
  pixel "$((x+w/4))" "$((y+h+6))"
  if ! (( g > 25 && r < 5 && b < 5 )); then
    echo "$1: missing bright shadow beneath visible half: $r $g $b"; exit 1
  fi
  pixel "$((x+3*w/4))" "$((y+h+6))"
  if ! (( g < 5 )); then
    echo "$1: full-size shadow ahead of hidden half: $r $g $b"; exit 1
  fi
  pixel "$((x+w/2+6))" "$((y+h/2))"
  if ! (( g > 25 && r < 5 && b < 5 )); then
    echo "$1: missing shadow at shader-created silhouette edge: $r $g $b"; exit 1
  fi
}
assert_half_shadow opening
# The blur must interpolate its kernel, not produce one flat band per tap.
changes=0
previous=-1
for distance in $(seq 3 14); do
  pixel "$((x+w/2+distance))" "$((y+h/2))"
  if [[ $g != "$previous" ]]; then changes=$((changes+1)); fi
  previous=$g
done
if (( changes < 10 )); then
  echo "silhouette blur has stepped bands: only $changes shades across 12 pixels"; exit 1
fi
"$UMBRIEL" clock-advance 2000
"$UMBRIEL" settle
grim "$IMAGE"
pixel "$((x+3*w/4))" "$((y+h+6))"
if ! (( g > 25 && r < 5 && b < 5 )); then
  echo "settled window did not restore its full analytic shadow: $r $g $b"; exit 1
fi
"$UMBRIEL" msg "window-close:$id" > /dev/null
"$UMBRIEL" clock-advance 200
assert_half_shadow closing
"$UMBRIEL" clock-advance 2000
"$UMBRIEL" settle
grim "$IMAGE"
green=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.05 && r < 0.01 && b < 0.01')
if [[ $green != 0 ]]; then
  echo "shadow survived its closing snapshot: $green pixels"; exit 1
fi
echo "bright shadows followed opening and closing silhouettes, restored, and cleaned up"
