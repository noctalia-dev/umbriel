#!/usr/bin/env bash
# Focus changes must interpolate each border from its current color instead of
# snapping it to the new focus color before the animation is retargeted. An
# overview round trip must reveal the borders settled rather than replaying.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/border-focus-transition.png"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"

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
[animation.overview]
duration_ms = 400

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
"$UMBRIEL" settle
grim "$IMAGE"
assert_color focus-border-a red
assert_color focus-border-b blue

# Animation time only moves by clock-advance; advancing 1000 ms finishes every border and overview timeline.
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg "window-focus:$b_id" > /dev/null
"$UMBRIEL" clock-advance 350
grim "$IMAGE"
assert_color focus-border-a mixed
assert_color focus-border-b mixed

# Opening the overview clears focus on the hidden windows and closing restores it; the reveal must show the settled
# result instead of the transition.
overview_round_trip() {
  "$UMBRIEL" clock-advance 1000
  "$UMBRIEL" settle
  "$UMBRIEL" msg overview-open > /dev/null
  "$UMBRIEL" clock-advance 600
  "$UMBRIEL" msg overview-close > /dev/null
  "$UMBRIEL" clock-advance 600
  grim "$IMAGE"
  assert_color focus-border-a blue
  assert_color focus-border-b red
}
overview_round_trip

# A shell's overview-scoped capture layer holds the keyboard exclusively while the overview is open and is dropped
# on the closed event. That event must go out as the close starts, so focus returns while the windows are still
# hidden.
"$UMBRIEL" subscribe overview | while read -r event; do
  if [[ $(jq -r .data.open <<< "$event") == true ]]; then
    "$LAYER_CLIENT" HEADLESS-1 0 bottom-layer keyboard=exclusive > /dev/null 2>&1 &
    capture=$!
  elif [[ -n ${capture:-} ]]; then
    kill "$capture"
    capture=
  fi
done &
overview_round_trip

"$UMBRIEL" clock-advance 1000
"$UMBRIEL" clock-resume

# A zero-width border has nothing to fade, so a focus change leaves no animation running and settle succeeds on a
# frozen clock.
sed -i 's/^border_width = 20$/border_width = 0/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg "window-focus:$a_id" > /dev/null
if ! timeout 5 "$UMBRIEL" settle; then
  echo "a focus change animated a zero-width border"
  exit 1
fi
"$UMBRIEL" clock-resume

echo "focus border colors interpolate during focus changes and settle across the overview"
