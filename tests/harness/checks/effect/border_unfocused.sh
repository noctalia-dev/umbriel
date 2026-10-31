#!/usr/bin/env bash
# border_unfocused = true widens the border-effect draw gate from the focused window to every decorated, calm,
# non-fullscreen window: each unfocused window paints its own selected preset (the reporter's focused/unfocused
# shader pair), and with the overview open every card paints its window's preset. With the flag off (the default)
# the gate stays focused-only and bit-identical — effect/border.sh owns that path.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/effect-border-unfocused.png"
cat > "$UMBRIEL_RUNTIME_DIR/focused-ring.glsl" <<'GLSL'
// Solid red ring. The green term is invisible at 8 bits but keeps umbriel_time an active uniform.
vec4 border(vec2 uv) { return vec4(1.0, 0.001 * sin(umbriel_time), 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/unfocused-ring.glsl" <<'GLSL'
// Solid green ring. The blue term keeps umbriel_time alive for the same reason.
vec4 border(vec2 uv) { return vec4(0.0, 1.0, 0.001 * sin(umbriel_time), 1.0); }
GLSL

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
[colors]
backdrop = "#000000FF"
[overview]
zoom = 0.5
[effects]
border_unfocused = true
[effects.preset.focused-ring]
kind = "border"
shader = "focused-ring.glsl"
padding = 20
[effects.preset.unfocused-ring]
kind = "border"
shader = "unfocused-ring.glsl"
padding = 20
[[window_rule]]
match.title = "^border-unfocused-one$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^border-unfocused-two$"
default_floating = true
default_position = { x = 700, y = 100, anchor = "top_left" }
[[window_rule]]
match.is_focused = true
border_effect = "focused-ring"
[[window_rule]]
match.is_focused = false
border_effect = "unfocused-ring"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn() {
  FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" "$1" 300 200 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]]
}
red_at() { "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.9 && g < 0.1 && b < 0.1' "$1"; }
green_at() { "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.9 && r < 0.1 && b < 0.2' "$1"; }

spawn border-unfocused-one
"$UMBRIEL" settle > /dev/null
read -r x1 y1 w1 _ < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
spawn border-unfocused-two
"$UMBRIEL" settle > /dev/null
read -r x2 y2 w2 _ < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
grim "$IMAGE"
# The last-spawned window is focused: its padding paints the focused preset.
if (( $(red_at "2x2+$((x2 + w2 / 2))+$((y2 - 12))") < 4 )); then
  echo "focused window did not paint its focused preset"
  exit 1
fi
# The deliverable: the unfocused window paints its own unfocused preset in the
# same 24 px band (4 px ring + 20 px padding). Without border_unfocused this
# padding is empty.
if (( $(green_at "2x2+$((x1 + w1 / 2))+$((y1 - 12))") < 4 )); then
  echo "unfocused window did not paint its unfocused preset"
  exit 1
fi
if (( $(green_at "2x2+$((x2 + w2 / 2))+$((y2 - 12))") > 0 )); then
  echo "unfocused preset leaked onto the focused window"
  exit 1
fi

# Overview cards follow the same gate. Opening the overview clears the seat, so
# both windows re-resolve to the unfocused preset and both cards paint green.
# The previously unfocused card is the deliverable probe: it draws only through
# allowUnfocused. The previously focused card keeps the workspace's focused
# gate, so its probe covers that pre-existing path, not the widening.
# The workspace row is the output box scaled by zoom (0.5) and centered, so a
# window at (x,y) lands at (row_x + x/2, row_y + y/2) and its padding band
# scales to 12 px (4 px ring + 20 px padding at half zoom).
out_size=$("$UMBRIEL" outputs | awk '/Usable area:/ {split($3, a, /[x+]/); print a[1], a[2]; exit}')
read -r out_w out_h <<< "$out_size"
if [[ -z $out_w || -z $out_h ]]; then
  echo "could not read the output size from: $("$UMBRIEL" outputs)"
  exit 1
fi
row_x=$(((out_w - out_w / 2) / 2))
row_y=$(((out_h - out_h / 2) / 2))
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
if (( $(green_at "2x2+$((row_x + x1 / 2 + w1 / 4))+$((row_y + y1 / 2 - 6))") < 4 )); then
  echo "overview card of the previously unfocused window paints no preset"
  exit 1
fi
if (( $(green_at "2x2+$((row_x + x2 / 2 + w2 / 4))+$((row_y + y2 / 2 - 6))") < 4 )); then
  echo "overview card of the previously focused window paints no preset"
  exit 1
fi

echo "border effects draw on unfocused windows and their overview cards when border_unfocused is on"
