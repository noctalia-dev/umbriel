#!/usr/bin/env bash
# harness: outputs=1
# Border details and overlays follow overview zoom, including animation, fractional output scale, and snapshots.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/overview-effect-scale.png"
cat > "$UMBRIEL_RUNTIME_DIR/ring.glsl" <<'GLSL'
vec4 border(vec2 uv) {
  return umbriel_border_distance(uv) < 20.0 ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0);
}
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/overlay.glsl" <<'GLSL'
vec4 window(vec2 uv) {
  float edge = min(uv.x, 1.0 - uv.x) * umbriel_size.x;
  return edge < 40.0 ? vec4(0.0, 1.0, 0.0, 1.0) : umbriel_sample(uv);
}
GLSL
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 40
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
[colors]
backdrop = "#000000FF"
[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"
[overview]
zoom = 0.5
[effects]
border = "ring"
in_capture = true
[effects.preset.ring]
kind = "border"
shader = "ring.glsl"
animated = false
padding = 40
overlay = "overlay"
[effects.preset.overlay]
kind = "window"
shader = "overlay.glsl"
[[window_rule]]
match.title = "^overview-effect-scale$"
default_floating = true
default_position = { x = 80, y = 80, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" overview-effect-scale 400 300 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "overview-effect-scale")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
"$UMBRIEL" settle > /dev/null

measure() {
  local label=$1 zoom=$2 output_scale=${3:-1} x y w h image_w image_h green red expected_green expected_red
  grim "$IMAGE"
  read -r x y w h < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.9 || g > 0.9')
  if (( w == 0 || h == 0 )); then
    echo "$label: no client pixels"
    exit 1
  fi
  IFS=x read -r image_w image_h < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" size)
  local row="${image_w}x1+0+$((y + h / 2))"
  green=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.9 && r < 0.1 && b < 0.1' "$row")
  red=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.9 && g < 0.1 && b < 0.1' "$row")
  expected_green=$(awk -v z="$zoom" -v s="$output_scale" 'BEGIN {printf "%.0f", 80*z*s}')
  expected_red=$(awk -v z="$zoom" -v s="$output_scale" 'BEGIN {printf "%.0f", 40*z*s}')
  if (( green < expected_green - 2 || green > expected_green + 2 )); then
    echo "$label: inner overlay is $green pixels wide, expected $expected_green"
    exit 1
  fi
  if (( red < expected_red - 3 || red > expected_red + 3 )); then
    echo "$label: border details are $red pixels wide, expected $expected_red"
    exit 1
  fi
  echo "$label: overlay=$green border=$red"
}

measure desktop 1
for zoom in 0.5 0.1; do
  sed -i "s/^zoom = .*/zoom = $zoom/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" msg overview-open > /dev/null
  "$UMBRIEL" settle > /dev/null
  measure "overview $zoom" "$zoom"
  "$UMBRIEL" msg overview-close > /dev/null
  "$UMBRIEL" settle > /dev/null
  measure "desktop after $zoom" 1
done

sed -i '/^\[animation\]/{n;s/enabled = false/enabled = true/;}' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation.overview]
enabled = true
duration_ms = 1000
curve = "linear"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" clock-advance 500
measure "opening halfway" 0.55
"$UMBRIEL" clock-advance 500
"$UMBRIEL" settle > /dev/null
measure "opening finished" 0.1
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" clock-advance 500
measure "closing halfway" 0.55
"$UMBRIEL" clock-advance 500
"$UMBRIEL" settle > /dev/null
measure "closing finished" 1
"$UMBRIEL" clock-resume

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output."HEADLESS-1"]
scale = 1.25
transform = "90"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
measure "rotated desktop" 1 1.25
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
measure "rotated overview 0.1" 0.1 1.25

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation.windows_out]
enabled = true
style = "fade"
duration_ms = 5000
curve = "linear"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" clock-freeze
id=$(jq -r '.id' <<< "$window")
"$UMBRIEL" msg "window-close:$id" > /dev/null
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 0 ]] && break
  sleep 0.02
done
[[ $("$UMBRIEL" windows --json | jq 'length') -eq 0 ]]
"$UMBRIEL" clock-advance 1
measure "overview close snapshot" 0.1 1.25
"$UMBRIEL" clock-advance 5000
"$UMBRIEL" clock-resume
