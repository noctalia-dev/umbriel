#!/usr/bin/env bash
# harness: outputs=1
# Border uv and umbriel_border_distance are logical on a rotated, fractionally scaled output: the left half of the ring
# (logical x) paints red and the right half blue, and the distance reads zero along the client edge.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/effect-border-transform.png"
cat > "$UMBRIEL_RUNTIME_DIR/halves.glsl" <<'GLSL'
vec4 border(vec2 uv) {
  float d = umbriel_border_distance(uv);
  // Green marks the first two logical pixels outside the client edge; red/blue halves elsewhere.
  if (d >= 0.0 && d < 2.0) return vec4(0.0, 1.0, 0.0, 1.0);
  return uv.x < 0.5 ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 0.0, 1.0, 1.0);
}
GLSL
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output."HEADLESS-1"]
scale = 1.25
transform = "90"
[animation]
enabled = false
[appearance]
border_width = 12
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
[effects]
border = "halves"
[effects.preset.halves]
kind = "border"
shader = "halves.glsl"
[[window_rule]]
match.title = "^transform$"
default_floating = true
# An explicit position keeps the window inside the rotated, scaled output's logical bounds (576x1024 here);
# default centering is not what this check exercises.
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFFFFFFFF "$UMBRIEL_UNMAP_CLIENT" transform 300 200 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "transform")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
"$UMBRIEL" settle > /dev/null
grim -s 1 "$IMAGE"
# The screenshot is in logical orientation. Sample the ring 6 px left of the client's left edge (red half) and 6 px
# right of its right edge (blue half), and the first pixel outside the top edge (green).
read -r x y w h < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
red=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.9 && b < 0.1 && g < 0.1' "2x2+$((x - 8))+$((y + h / 2))")
blue=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'b > 0.9 && r < 0.1 && g < 0.1' "2x2+$((x + w + 6))+$((y + h / 2))")
green=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.9 && r < 0.1 && b < 0.1' "2x1+$((x + w / 2))+$((y - 1))")
if (( red < 4 || blue < 4 )); then
  echo "border uv was not logical on the rotated fractional output: red=$red blue=$blue"
  exit 1
fi
if (( green < 1 )); then
  echo "umbriel_border_distance did not read zero at the client edge: green=$green"
  exit 1
fi
echo "border uv halves and border distance survived rotation and fractional scale"
