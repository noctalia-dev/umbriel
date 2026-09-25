#!/usr/bin/env bash
# A shader-shaped shadow falls on the window below its caster, avoids tinting translucent content, and enters the
# enclosing workspace shader exactly once.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/composition.png"
readonly BASE="$UMBRIEL_RUNTIME_DIR/base.toml"
cp "$UMBRIEL_CONFIG" "$BASE"
cat > "$UMBRIEL_RUNTIME_DIR/half.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
    float a = uv.x < 0.5 ? umbriel_sample(uv).a * 0.5 : 0.0;
    return vec4(0.0, 0.0, a, a);
}
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/outer.glsl" <<'GLSL'
vec4 animation(vec2 uv) { vec4 c = umbriel_sample(uv); return vec4(c.g, 0.0, 0.0, c.a); }
GLSL
configure() {
  cat "$BASE" > "$UMBRIEL_CONFIG"
  cat >> "$UMBRIEL_CONFIG" <<EOF

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
duration_ms = 4000
curve = "linear"
[animation.windows_in]
enabled = $1
shader = "half.glsl"
[animation.windows_out]
enabled = false
[animation.windows_move]
enabled = false
[animation.workspaces]
shader = "outer.glsl"
[[window_rule]]
match.title = "^occluder$"
default_floating = true
default_position = { x = 510, y = 220, anchor = "top_left" }
[[window_rule]]
match.title = "^caster$"
default_floating = true
default_position = { x = 200, y = 120, anchor = "top_left" }
EOF
  "$UMBRIEL" msg config-reload > /dev/null
}
spawn() {
  "$UMBRIEL_UNMAP_CLIENT" "$1" "$2" "$3" > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]]
}
pixel() {
  read -r r g b < <(magick "$IMAGE" -crop "2x2+$1+$2" \
    -format '%[fx:round(mean.r*255)] %[fx:round(mean.g*255)] %[fx:round(mean.b*255)]\n' info:)
}
configure false
spawn occluder 180 180
occluder=$(jq -r .id <<< "$window")
configure true
# Animation time only moves by clock-advance; each sample lands 200 ms into a 4000 ms timeline.
"$UMBRIEL" clock-freeze
spawn caster 700 400
"$UMBRIEL" clock-advance 200
grim "$IMAGE"
pixel 556 170
if ! (( g > 15 && r < 5 && b < 5 )); then
  echo "missing silhouette shadow outside the overlapping window: $r $g $b"; exit 1
fi
# The caster is above the occluder, so its silhouette shadow falls on it. Compare with an occluder point the silhouette
# cannot reach.
pixel 556 270
read -r sr sg sb <<< "$r $g $b"
pixel 668 388
if ! (( sg > g + 10 && sr <= r && sb <= b )); then
  echo "silhouette shadow missing from the window below: $sr $sg $sb against $r $g $b"; exit 1
fi
pixel 540 170
if ! (( r < 5 && g < 5 && b > 115 && b < 140 )); then
  echo "shadow tinted translucent caster content: $r $g $b"; exit 1
fi
"$UMBRIEL" msg "window-close:$occluder" > /dev/null
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" clock-advance 200
grim "$IMAGE"
red=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.04 && g < 0.01 && b < 0.01')
green=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.04 && r < 0.01 && b < 0.01')
if ! (( red > 50 && green == 0 )); then
  echo "workspace shader did not process the shadow exactly once: red=$red green=$green"; exit 1
fi
echo "shadow stacking, translucent interior exclusion, and enclosing workspace composition verified"
