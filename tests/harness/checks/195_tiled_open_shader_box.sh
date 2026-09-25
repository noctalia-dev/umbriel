#!/usr/bin/env bash
# A tiled opener shows its windows_in shader over its final slot while the neighbour that vacates the slot is still
# moving underneath it. Neither a collapsed opening box nor windows_move shader composition may replace that effect.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/tiled-open-shader-box.png"

cat > "$UMBRIEL_RUNTIME_DIR/open.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
    vec4 source = umbriel_sample(uv);
    return source.b > 0.5 ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 1.0, 1.0);
}
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/move.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[layout]
mode = "master"

[animation.windows_in]
enabled = true
duration_ms = 1000
curve = "linear"
style = "none"
shader = "open.glsl"

[animation.windows_out]
enabled = false

[animation.windows_move]
enabled = true
duration_ms = 1000
curve = "linear"
shader = "move.glsl"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn() {
  local title=$1
  FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" "$title" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
    [[ -n $window ]] && return 0
    sleep 0.025
  done
  echo "timed out waiting for $title"
  return 1
}

red_pixels() {
  "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.8 && g < 0.1 && b < 0.1'
}

sample_center() {
  local description=$1 json=$2
  local x y red green blue moving
  # IPC reports the final target origin but may still expose committed client size. An inset from that origin stays
  # inside either stack row, including the default new-on-top placement of the third window.
  x=$(jq -r '.x + 100' <<< "$json")
  y=$(jq -r '.y + 100' <<< "$json")
  grim "$IMAGE"
  read -r red green blue < <(
    magick "$IMAGE" -alpha off -crop "8x8+$((x - 4))+$((y - 4))" +repage \
      -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]\n' info:
  )
  if ! ((red < 40 && green > 200 && blue < 40)); then
    echo "$description did not show its windows_in shader over its final slot: $red $green $blue"
    exit 1
  fi
  moving=$(red_pixels)
  if ((moving < 1000)); then
    echo "$description was sampled after the neighbour's windows_move shader had ended: red_pixels=$moving"
    exit 1
  fi
}

spawn tiled-shader-first
"$UMBRIEL" settle

# Animation time only moves by clock-advance: samples land 150 ms and 600 ms into both 1000 ms timelines.
"$UMBRIEL" clock-freeze
spawn tiled-shader-second
second=$window
"$UMBRIEL" clock-advance 150
sample_center "second tiled opener" "$second"
"$UMBRIEL" clock-advance 450
sample_center "second tiled opener" "$second"
"$UMBRIEL" clock-advance 1000
"$UMBRIEL" settle

spawn tiled-shader-third
third=$window
"$UMBRIEL" clock-advance 150
sample_center "third tiled opener" "$third"
"$UMBRIEL" clock-advance 450
sample_center "third tiled opener" "$third"

echo "each tiled opener showed its windows_in shader in its final slot while its neighbours reflowed"
