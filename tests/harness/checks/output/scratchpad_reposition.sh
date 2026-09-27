#!/usr/bin/env bash
# harness: outputs=2
# A scratchpad assigned to an enabled output follows that output when its
# layout position changes. Its output-local geometry and backdrop move with it.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
readonly TITLE=scratchpad-output-reposition
readonly NAME=reposition
readonly FIRST_SHOT="$UMBRIEL_RUNTIME_DIR/reposition-first.png"
readonly SECOND_SHOT="$UMBRIEL_RUNTIME_DIR/reposition-second.png"

write_config() {
  local second_mode=${3:-1280x720}
  local second_scale=${4:-1.0}
  {
    printf '%s\n' "$BASELINE"
    printf '\n[animation]\nenabled = false\n'
    printf '\n[animation.scratchpad]\nenabled = false\ndim = 0.5\nblur = false\nscale = 0.0\n'
    printf '\n[colors]\nbackdrop = "#FFFFFFFF"\n'
    printf '\n[appearance]\nborder_width = 0\nouter_border_width = 0\ncorner_radius = 0\n'
    printf '\n[output.HEADLESS-1]\nenabled = true\nmode = "1280x720"\nposition = [%s, 0]\nworkspaces = ["LEFT"]\n' "$1"
    printf '\n[output.HEADLESS-2]\nenabled = true\nmode = "%s"\nscale = %s\nposition = [%s, 0]\nworkspaces = ["RIGHT"]\n' "$second_mode" "$second_scale" "$2"
    printf '\n[[scratchpad]]\nname = "%s"\n' "$NAME"
    printf '\n[[window_rule]]\nmatch.title = "^%s$"\ndefault_output = "HEADLESS-2"\ndefault_floating = true\ndefault_floating_size_px = { width = 360, height = 220 }\ndefault_position = { x = 240, y = 180, anchor = "top_left" }\n' "$TITLE"
  } > "$UMBRIEL_CONFIG"
}

windows() { "$UMBRIEL" windows --json; }

field_of() {
  windows | jq -r --arg title "$TITLE" --arg field "$1" '.[] | select(.title == $title) | .[$field]'
}

wait_for_count() {
  local expected=$1 count=
  for _ in $(seq 80); do
    count=$(windows | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected window, got $count: $(windows)"
  return 1
}

wait_for_field() {
  local field=$1 expected=$2 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$TITLE' field '$field' to be '$expected', got '$actual': $(windows)"
  return 1
}

output_origin() {
  "$UMBRIEL" outputs \
    | awk -v name="$1" '$1 == name { found = 1; next } found && /Position:/ { split($2, p, ","); print p[1], p[2]; exit }'
}

wait_for_output_origin() {
  local name=$1 expected_x=$2 expected_y=$3 actual_x= actual_y=
  for _ in $(seq 80); do
    read -r actual_x actual_y < <(output_origin "$name")
    [[ $actual_x == "$expected_x" && $actual_y == "$expected_y" ]] && return 0
    sleep 0.1
  done
  echo "expected $name at $expected_x,$expected_y, got $actual_x,$actual_y"
  "$UMBRIEL" outputs
  return 1
}

sample_rgb() {
  local image=$1 x=$2 y=$3
  magick "$image" -crop "20x20+$((x - 10))+$((y - 10))" \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

assert_content() {
  local label=$1 image=$2 red green blue
  read -r red green blue <<< "$(sample_rgb "$image" 420 290)"
  if ((red > 30 || green < 80 || blue < 80)); then
    echo "$label does not contain the scratchpad client: red=$red green=$green blue=$blue"
    return 1
  fi
}

assert_no_content() {
  local label=$1 image=$2 red green blue
  read -r red green blue <<< "$(sample_rgb "$image" 420 290)"
  if ((red < 200)); then
    echo "$label still contains the scratchpad client or backdrop: red=$red green=$green blue=$blue"
    return 1
  fi
}

assert_dimmed() {
  local label=$1 image=$2 red green blue
  read -r red green blue <<< "$(sample_rgb "$image" 20 20)"
  if ((red < 100 || red > 160 || green < 100 || green > 160 || blue < 100 || blue > 160)); then
    echo "$label backdrop is not dimmed: red=$red green=$green blue=$blue"
    return 1
  fi
}

assert_bright() {
  local label=$1 image=$2 red green blue
  read -r red green blue <<< "$(sample_rgb "$image" 20 20)"
  if ((red < 230 || green < 230 || blue < 230)); then
    echo "$label backdrop is not clear: red=$red green=$green blue=$blue"
    return 1
  fi
}

write_config 0 1280
"$UMBRIEL" msg config-reload > /dev/null
wait_for_output_origin HEADLESS-1 0 0
wait_for_output_origin HEADLESS-2 1280 0
"$POINTER" 2560 720 move 1920 360

"$CLIENT" "$TITLE" > "$UMBRIEL_RUNTIME_DIR/$TITLE.log" 2>&1 &
wait_for_count 1
"$UMBRIEL" msg "window-focus:$(field_of id)" > /dev/null
"$UMBRIEL" msg "window-move-to-scratchpad:$NAME" > /dev/null
"$UMBRIEL" msg "scratchpad-toggle:$NAME" > /dev/null
wait_for_field scratchpad "$NAME"
wait_for_field workspace ""
wait_for_field x 1520
wait_for_field y 180
wait_for_field w 360
wait_for_field h 220
wait_for_field active true

grim -o HEADLESS-2 "$FIRST_SHOT"
assert_content "HEADLESS-2 before the layout change" "$FIRST_SHOT"
assert_dimmed "HEADLESS-2 before the layout change" "$FIRST_SHOT"

# Changing only the scale must preserve keyboard focus on the visible
# scratchpad instead of selecting or clearing the workspace focus beneath it.
write_config 0 1280 1280x720 1.25
"$UMBRIEL" msg config-reload > /dev/null
wait_for_output_origin HEADLESS-2 1280 0
wait_for_field x 1472
wait_for_field y 144
wait_for_field active true

# Swap the live outputs without touching the scratchpad. The same Output object
# remains its owner, so its output-local geometry must stay at 240,180.
write_config 1280 0
"$UMBRIEL" msg config-reload > /dev/null
wait_for_output_origin HEADLESS-1 1280 0
wait_for_output_origin HEADLESS-2 0 0
wait_for_field scratchpad "$NAME"
wait_for_field workspace ""
wait_for_field x 240
wait_for_field y 180
wait_for_field w 360
wait_for_field h 220

grim -o HEADLESS-1 "$FIRST_SHOT"
grim -o HEADLESS-2 "$SECOND_SHOT"
assert_no_content "HEADLESS-1 after the layout change" "$FIRST_SHOT"
assert_bright "HEADLESS-1 after the layout change" "$FIRST_SHOT"
assert_content "HEADLESS-2 after the layout change" "$SECOND_SHOT"
assert_dimmed "HEADLESS-2 after the layout change" "$SECOND_SHOT"

"$UMBRIEL" msg "scratchpad-focus-next:$NAME" > /dev/null
wait_for_field active true

# A maximized scratchpad tracks a live mode change, while its saved floating
# box retains the same proportional output-local position.
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_field x 0
wait_for_field y 0
wait_for_field w 1280
wait_for_field h 720
write_config 960 0 960x540
"$UMBRIEL" msg config-reload > /dev/null
wait_for_output_origin HEADLESS-1 960 0
wait_for_output_origin HEADLESS-2 0 0
wait_for_field x 0
wait_for_field y 0
wait_for_field w 960
wait_for_field h 540
"$UMBRIEL" msg "scratchpad-focus-next:$NAME" > /dev/null
wait_for_field active true
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_field x 180
wait_for_field y 135
wait_for_field w 360
wait_for_field h 220

"$UMBRIEL" msg "window-restore-from-scratchpad:$NAME" > /dev/null
wait_for_field scratchpad ""
for _ in $(seq 80); do
  workspace=$(field_of workspace)
  [[ $workspace == HEADLESS-2:* ]] && break
  sleep 0.1
done
if [[ $workspace != HEADLESS-2:* ]]; then
  echo "restored window lost its saved HEADLESS-2 workspace: $(windows)"
  exit 1
fi

echo "scratchpad geometry, backdrop, and focus followed enabled output changes"
