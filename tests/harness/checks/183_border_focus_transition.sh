#!/usr/bin/env bash
# Focus changes must interpolate each border from its current color instead of
# snapping it to the new focus color before the animation is retargeted.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/border-focus-transition.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

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
match.title = "^focus-border-a$"
default_floating = true
default_position = { x = 200, y = 120, anchor = "top_left" }

[[window_rule]]
match.title = "^focus-border-b$"
default_floating = true
default_position = { x = 800, y = 120, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

for title in focus-border-a focus-border-b; do
  "$UMBRIEL_UNMAP_CLIENT" "$title" 300 300 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]]
done

border_position() {
  local title=$1
  local window
  window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
  jq -r '"\(.x + (.w / 2 | floor)) \(.y - 8)"' <<< "$window"
}

sample_border() {
  local title=$1 x y
  read -r x y < <(border_position "$title")
  magick "$IMAGE" -crop "4x4+$x+$y" \
    -format '%[fx:round(mean.r*255)] %[fx:round(mean.g*255)] %[fx:round(mean.b*255)]\n' info:
}

assert_color() {
  local label=$1 expected=$2 r g b
  read -r r g b < <(sample_border "$label")
  case $expected in
    red) (( r > 220 && g < 20 && b < 20 )) ;;
    blue) (( b > 220 && r < 20 && g < 20 )) ;;
    mixed) (( r > 20 && b > 20 )) ;;
  esac || {
    echo "$label border was not $expected: $r $g $b"
    exit 1
  }
}

window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "focus-border-a")')
a_id=$(jq -r .id <<< "$window")
window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "focus-border-b")')
b_id=$(jq -r .id <<< "$window")

"$UMBRIEL" msg "window-focus:$a_id" > /dev/null
sleep 1.1
grim "$IMAGE"
assert_color focus-border-a red
assert_color focus-border-b blue

"$UMBRIEL" msg "window-focus:$b_id" > /dev/null
sleep 0.35
grim "$IMAGE"
assert_color focus-border-a mixed
assert_color focus-border-b mixed

echo "focus border colors interpolate during focus changes"
