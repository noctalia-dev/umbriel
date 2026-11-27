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
[effects.preset.border_border]
kind = "animation"
shader = "border.glsl"
[animation.border]
enabled = true
effect = "border_border"
[effects.preset.windows_out_identity]
kind = "animation"
shader = "identity.glsl"
[animation.windows_out]
effect = "windows_out_identity"
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
  read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" mean "2x2+$x+$y")
  if ! (( g > 25 && r < 5 && b < 5 )); then
    echo "$1: border shader shadow did not follow the content edge: $r $g $b"; exit 1
  fi
}
"$UMBRIEL" msg window-focus "$id" > /dev/null
"$UMBRIEL" clock-advance 200
assert_shadow focus
"$UMBRIEL" msg window-close "$id" > /dev/null
"$UMBRIEL" clock-advance 200
assert_shadow closing

# A clear client still has an analytic shadow while mapped. Its closing fade
# tracks the captured alpha.
sed -i '
  /^\[animation.border\]$/,/^\[/s/^enabled = true$/enabled = false/
  s/^effect = "windows_out_identity"$/effect = ""/
  /^\[animation.windows_out\]$/a style = "fade"
' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^clear-shadow$"
border_width = 0
outer_border_width = 0
default_floating = true
default_position = { x = 300, y = 500, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0x00000000 "$UMBRIEL_UNMAP_CLIENT" clear-shadow 400 160 > "$UMBRIEL_RUNTIME_DIR/clear.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "clear-shadow")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
id=$(jq -r .id <<< "$window")
x=$(jq -r '.x + (.w / 2 | floor)' <<< "$window")
y=$(jq -r '.y - 6' <<< "$window")
"$UMBRIEL" clock-advance 2000
assert_shadow "clear mapped client"
"$UMBRIEL" msg window-close "$id" > /dev/null
unmapped=0
for _ in $(seq 80); do
  if grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/clear.log" \
      && ! "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "clear-shadow")' > /dev/null; then
    unmapped=1
    break
  fi
  sleep 0.025
done
if (( ! unmapped )); then
  echo "clear client did not acknowledge unmap after close"; exit 1
fi
"$UMBRIEL" clock-advance 200
grim "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" mean "2x2+$x+$y")
if (( g >= 5 )); then
  echo "transparent close snapshot retained an analytic shadow: $r $g $b"; exit 1
fi
echo "descendant-only closing silhouettes and clear-client closing shadows verified"
