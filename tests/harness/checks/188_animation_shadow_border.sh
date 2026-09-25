#!/usr/bin/env bash
# A descendant-only effect changes the caster too. Freeze it on close so the
# removed border and its enlarged analytic shadow do not flash back into view.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/border.png"
cat > "$UMBRIEL_RUNTIME_DIR/border.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return vec4(0.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/identity.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return umbriel_sample(uv); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
shadow = "#00FF00FF"
[colors.border]
focused = "#FF0000"
unfocused = "#800000"
[appearance]
border_width = 20
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
[animation.windows_in]
enabled = false
[animation.windows_move]
enabled = false
[animation.border]
enabled = true
shader = "border.glsl"
[animation.windows_out]
shader = "identity.glsl"
[[window_rule]]
match.title = "^border-a$"
default_floating = true
default_position = { x = 200, y = 120, anchor = "top_left" }
[[window_rule]]
match.title = "^border-b$"
default_floating = true
default_position = { x = 800, y = 120, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
# Animation time only moves by clock-advance: samples land 200 ms into each 2000 ms timeline.
"$UMBRIEL" clock-freeze
for title in border-a border-b; do
  "$UMBRIEL_UNMAP_CLIENT" "$title" 300 300 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]]
done
"$UMBRIEL" clock-advance 2000
"$UMBRIEL" settle
window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "border-a")')
id=$(jq -r .id <<< "$window")
x=$(jq -r '.x + (.w / 2 | floor)' <<< "$window")
y=$(jq -r '.y - 6' <<< "$window")
assert_shadow() {
  grim "$IMAGE"
  read -r r g b < <(magick "$IMAGE" -crop "2x2+$x+$y" \
    -format '%[fx:round(mean.r*255)] %[fx:round(mean.g*255)] %[fx:round(mean.b*255)]\n' info:)
  if ! (( g > 25 && r < 5 && b < 5 )); then
    echo "$1: border shader shadow did not follow the content edge: $r $g $b"; exit 1
  fi
}
"$UMBRIEL" msg "window-focus:$id" > /dev/null
"$UMBRIEL" clock-advance 200
assert_shadow focus
"$UMBRIEL" msg "window-close:$id" > /dev/null
"$UMBRIEL" clock-advance 200
assert_shadow closing
echo "descendant-only border silhouette and its closing snapshot shadow verified"
