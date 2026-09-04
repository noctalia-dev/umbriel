#!/usr/bin/env bash
# harness: outputs=2
# Opening the overview hides every visible scratchpad, including scratchpads on outputs that do not hold focus. The
# windows stay stored after the overview closes, and one toggle per output shows them again. Transparent overview
# backgrounds make the scene transition observable instead of merely covering an incorrectly visible scratchpad.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OUTPUT_LAYOUT_W=2560
readonly OUTPUT_LAYOUT_H=720
readonly FIRST_TITLE=overview-scratch-first
readonly SECOND_TITLE=overview-scratch-second
readonly FIRST_OUTPUT=HEADLESS-1
readonly SECOND_OUTPUT=HEADLESS-2
readonly FIRST_LOG="$UMBRIEL_RUNTIME_DIR/overview-scratch-first.log"
readonly SECOND_LOG="$UMBRIEL_RUNTIME_DIR/overview-scratch-second.log"

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_workspace() {
  local title=$1 output=$2 workspace=
  for _ in $(seq 60); do
    workspace=$("$UMBRIEL" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | .workspace')
    [[ $workspace == "$output":* ]] && return 0
    sleep 0.1
  done
  echo "window '$title' did not map on $output: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_empty_workspace() {
  local title=$1 window= workspace=
  for _ in $(seq 60); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
    if [[ -n $window ]]; then
      workspace=$(jq -r '.workspace' <<< "$window")
      [[ -z $workspace ]] && return 0
    fi
    sleep 0.1
  done
  echo "window '$title' retained workspace '$workspace' after moving to the scratchpad: $("$UMBRIEL" windows --json)"
  return 1
}

output_origin() {
  "$UMBRIEL" outputs \
    | awk -v name="$1" '$1 == name { found = 1; next } found && /Position:/ { split($2, p, ","); print p[1], p[2]; exit }'
}

move_pointer_to_output() {
  local output=$1 output_x output_y
  read -r output_x output_y < <(output_origin "$output")
  if [[ -z ${output_x:-} || -z ${output_y:-} ]]; then
    echo "could not resolve origin for $output" >&2
    return 1
  fi
  "$POINTER" "$OUTPUT_LAYOUT_W" "$OUTPUT_LAYOUT_H" move "$((output_x + 640))" "$((output_y + 360))"
}

window_center() {
  local title=$1 output=$2 output_x output_y window_x window_y window_w window_h
  read -r output_x output_y < <(output_origin "$output")
  if [[ -z ${output_x:-} || -z ${output_y:-} ]]; then
    echo "could not resolve origin for $output" >&2
    return 1
  fi
  read -r window_x window_y window_w window_h < <(
    "$UMBRIEL" windows --json \
      | jq -r --arg title "$title" '.[] | select(.title == $title) | "\(.x) \(.y) \(.w) \(.h)"'
  )
  if [[ -z ${window_x:-} || -z ${window_y:-} || -z ${window_w:-} || -z ${window_h:-} ]]; then
    echo "could not resolve geometry for '$title': $("$UMBRIEL" windows --json)" >&2
    return 1
  fi
  printf '%d %d\n' "$((window_x - output_x + window_w / 2))" "$((window_y - output_y + window_h / 2))"
}

sample_rgb() {
  local image=$1 x=$2 y=$3
  magick "$image" -crop "20x20+$((x - 10))+$((y - 10))" -colorspace RGB \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

assert_blue() {
  local label=$1 image=$2 x=$3 y=$4 red green blue
  read -r red green blue <<< "$(sample_rgb "$image" "$x" "$y")"
  if ((blue < 100 || blue < red + 40)); then
    echo "$label scratchpad window is not visible: red=$red green=$green blue=$blue"
    return 1
  fi
}

assert_dark() {
  local label=$1 image=$2 x=$3 y=$4 red green blue
  read -r red green blue <<< "$(sample_rgb "$image" "$x" "$y")"
  if ((red > 15 || green > 15 || blue > 15)); then
    echo "$label scratchpad window remained visible: red=$red green=$green blue=$blue"
    return 1
  fi
}

capture() {
  local output=$1 name=$2
  local path="$UMBRIEL_RUNTIME_DIR/$name.png"
  grim -o "$output" "$path"
  printf '%s\n' "$path"
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.overview]
enabled = false

[animation.scratchpad]
enabled = false
dim = 0.0
blur = false
scale = 0.0

[colors]
backdrop = "#000000FF"

[colors.overview]
background_tint = "#00000000"
workspace_background = "#00000000"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.blur]
enabled = false

[overview]
background_blur = false
shortcuts = false

[[window_rule]]
match.title = "^overview-scratch-first$"
default_output = "HEADLESS-1"
default_floating = true
default_size = [420, 260]
default_position = { x = 120, y = 100, anchor = "top_left" }

[[window_rule]]
match.title = "^overview-scratch-second$"
default_output = "HEADLESS-2"
default_floating = true
default_size = [420, 260]
default_position = { x = 120, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" "$FIRST_TITLE" 420 260 > "$FIRST_LOG" 2>&1 &
wait_for_count 1
wait_for_workspace "$FIRST_TITLE" "$FIRST_OUTPUT"
move_pointer_to_output "$FIRST_OUTPUT"
"$UMBRIEL" msg "window-move-to-scratchpad:$FIRST_OUTPUT" > /dev/null
wait_for_empty_workspace "$FIRST_TITLE"

"$CLIENT" "$SECOND_TITLE" 420 260 > "$SECOND_LOG" 2>&1 &
wait_for_count 2
wait_for_workspace "$SECOND_TITLE" "$SECOND_OUTPUT"
move_pointer_to_output "$SECOND_OUTPUT"
"$UMBRIEL" msg "window-move-to-scratchpad:$SECOND_OUTPUT" > /dev/null
wait_for_empty_workspace "$SECOND_TITLE"

"$UMBRIEL" msg "scratchpad-toggle:$FIRST_OUTPUT" > /dev/null
"$UMBRIEL" msg "scratchpad-toggle:$SECOND_OUTPUT" > /dev/null
sleep 0.1

read -r first_x first_y < <(window_center "$FIRST_TITLE" "$FIRST_OUTPUT")
read -r second_x second_y < <(window_center "$SECOND_TITLE" "$SECOND_OUTPUT")

first_before=$(capture "$FIRST_OUTPUT" overview-scratch-first-before)
second_before=$(capture "$SECOND_OUTPUT" overview-scratch-second-before)
assert_blue "$FIRST_OUTPUT before overview" "$first_before" "$first_x" "$first_y"
assert_blue "$SECOND_OUTPUT before overview" "$second_before" "$second_x" "$second_y"

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.1
first_open=$(capture "$FIRST_OUTPUT" overview-scratch-first-open)
second_open=$(capture "$SECOND_OUTPUT" overview-scratch-second-open)
assert_dark "$FIRST_OUTPUT after overview-open" "$first_open" "$first_x" "$first_y"
assert_dark "$SECOND_OUTPUT after overview-open" "$second_open" "$second_x" "$second_y"

if [[ $("$UMBRIEL" windows --json | jq 'length') -ne 2 ]]; then
  echo "overview-open removed a stored scratchpad window: $("$UMBRIEL" windows --json)"
  exit 1
fi

"$UMBRIEL" msg overview-close > /dev/null
sleep 0.1
first_closed=$(capture "$FIRST_OUTPUT" overview-scratch-first-closed)
second_closed=$(capture "$SECOND_OUTPUT" overview-scratch-second-closed)
assert_dark "$FIRST_OUTPUT after overview-close" "$first_closed" "$first_x" "$first_y"
assert_dark "$SECOND_OUTPUT after overview-close" "$second_closed" "$second_x" "$second_y"

"$UMBRIEL" msg "scratchpad-toggle:$FIRST_OUTPUT" > /dev/null
sleep 0.1
first_restored=$(capture "$FIRST_OUTPUT" overview-scratch-first-restored)
second_still_hidden=$(capture "$SECOND_OUTPUT" overview-scratch-second-still-hidden)
assert_blue "$FIRST_OUTPUT after its first toggle" "$first_restored" "$first_x" "$first_y"
assert_dark "$SECOND_OUTPUT before its first toggle" "$second_still_hidden" "$second_x" "$second_y"

"$UMBRIEL" msg "scratchpad-toggle:$SECOND_OUTPUT" > /dev/null
sleep 0.1
second_restored=$(capture "$SECOND_OUTPUT" overview-scratch-second-restored)
assert_blue "$SECOND_OUTPUT after its first toggle" "$second_restored" "$second_x" "$second_y"

echo "overview-open hid both output scratchpads and preserved them for independent toggles"
