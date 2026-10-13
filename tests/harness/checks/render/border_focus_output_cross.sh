#!/usr/bin/env bash
# harness: outputs=2
# With follows_mouse, an output-focus action warps the pointer across outputs, which focuses the target before the
# action refocuses it. That repeated focus must leave both border transitions running instead of snapping them.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/border-focus-output-cross.png"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output.HEADLESS-1]
mode = "1280x720"
position = [0, 0]

[output.HEADLESS-2]
mode = "1280x720"
position = [1280, 0]

[input.focus]
follows_mouse = true

[colors]
backdrop = "#000000FF"
[colors.border]
focused = "#FF0000"
unfocused = "#0000FF"

[appearance]
border_width = 20
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false

[animation]
duration_ms = 1000
curve = "linear"
[animation.windows_in]
enabled = false
[animation.windows_move]
enabled = false
[animation.border]
enabled = true

[[window_rule]]
match.title = "^cross-border-a$"
default_output = "HEADLESS-1"
default_floating = true
default_position = { x = 200, y = 120, anchor = "top_left" }

[[window_rule]]
match.title = "^cross-border-b$"
default_output = "HEADLESS-2"
default_floating = true
default_position = { x = 200, y = 120, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

for output in HEADLESS-1:0 HEADLESS-2:1280; do
  for _ in $(seq 80); do
    x=$("$UMBRIEL" outputs --json | jq -r --arg name "${output%%:*}" '.[] | select(.name == $name) | .position.x')
    [[ $x == "${output##*:}" ]] && break
    sleep 0.025
  done
  [[ $x == "${output##*:}" ]]
done

window_json() { "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'; }

for title in cross-border-a cross-border-b; do
  "$UMBRIEL_UNMAP_CLIENT" "$title" 300 300 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 80); do
    window=$(window_json "$title")
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]]
done

assert_color() {
  local title=$1 expected=$2 window x y r g b
  window=$(window_json "$title")
  x=$(jq -r '.x + (.w / 2 | floor)' <<< "$window")
  y=$(jq -r '.y - 8' <<< "$window")
  read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" mean "4x4+$x+$y")
  case $expected in
    red) ((r > 220 && g < 20 && b < 20)) ;;
    blue) ((b > 220 && r < 20 && g < 20)) ;;
    mixed) ((r > 20 && b > 20)) ;;
  esac || {
    echo "$title border was not $expected: $r $g $b"
    exit 1
  }
}

# Hovering A focuses it and leaves the pointer on HEADLESS-1. The HEADLESS-2 center the action warps to is outside B,
# so only the output crossing and the action's own refocus reach B.
window=$(window_json cross-border-a)
"$POINTER" 2560 720 move "$(jq -r '.x + (.w / 2 | floor)' <<< "$window")" "$(jq -r '.y + (.h / 2 | floor)' <<< "$window")"
"$UMBRIEL" settle
grim "$IMAGE"
assert_color cross-border-a red
assert_color cross-border-b blue

"$UMBRIEL" clock-freeze
"$UMBRIEL" msg output-focus-next > /dev/null
"$UMBRIEL" clock-advance 350
grim "$IMAGE"
assert_color cross-border-a mixed
assert_color cross-border-b mixed

"$UMBRIEL" clock-advance 1000
grim "$IMAGE"
assert_color cross-border-a blue
assert_color cross-border-b red

"$UMBRIEL" msg output-focus-previous > /dev/null
"$UMBRIEL" clock-advance 350
grim "$IMAGE"
assert_color cross-border-a mixed
assert_color cross-border-b mixed

"$UMBRIEL" clock-advance 1000
"$UMBRIEL" clock-resume

echo "focus border colors interpolate when an output-focus action crosses outputs under follows_mouse"
