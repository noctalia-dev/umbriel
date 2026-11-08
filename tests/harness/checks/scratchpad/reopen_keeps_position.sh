#!/usr/bin/env bash
# harness: outputs=2
# A scratchpad never reframes a floating member it shows: a window hanging off an output edge but still grabbable keeps
# its place across a hide and a reopen. Movement belongs to the pad's output changes instead, which carry members to the
# new output: one the user moves into a pad showing elsewhere joins that output, one dropped with every output remains
# inside the output that replaces it, one hanging off a replaced output stays grabbable, and none of them is parked at an
# output's centre.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly PAD=pad
readonly SCALED_PAD=scaled
readonly EDGE_TITLE=edge-member
readonly ARRIVING_TITLE=arriving-member
readonly HANGING_TITLE=hanging-member
readonly SCALED_TITLE=scaled-member
readonly SHOTS="$UMBRIEL_RUNTIME_DIR/reopen-keeps-position"
# 400x240 at x = -300 leaves 100 px visible, inside the 75 px float margin, with its centre off the output.
readonly EDGE_X=-300
readonly EDGE_Y=100
readonly HANGING_Y=200
readonly SCALED_WIDTH=600
readonly SCALED_HEIGHT=400
readonly WIDTH=400
readonly HEIGHT=240
# The float margin keeps this many pixels of the frame on the output.
readonly MARGIN=75
mkdir -p "$SHOTS"

windows() { "$UMBRIEL" windows --json; }

field_of() {
  local title=$1 field=$2
  windows | jq -r --arg title "$title" --arg field "$field" '.[] | select(.title == $title) | .[$field]'
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected '$title' $field to be '$expected', got '$actual': $(windows)"
  return 1
}

wait_for_scratchpad() {
  local title=$1 expected=$2 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" scratchpad)
    [[ $actual == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected '$title' to belong to scratchpad '$expected', got '$actual': $(windows)"
  return 1
}

spawn() {
  local title=$1
  shift
  env FILL_COLOR=0xFF5577AA "$@" "$CLIENT" "$title" "$WIDTH" "$HEIGHT" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 100); do
    grep -q '^mapped$' "$UMBRIEL_RUNTIME_DIR/$title.log" && return 0
    sleep 0.025
  done
  echo "client $title never mapped: $(cat "$UMBRIEL_RUNTIME_DIR/$title.log")"
  exit 1
}

visible_pixels() {
  local image=$1 output=$2
  grim -o "$output" "$image"
  "$UMBRIEL_PIXEL_PROBE" "$image" count 'r < 0.45 && g > 0.28 && b > 0.55'
}

readonly BASELINE=$(< "$UMBRIEL_CONFIG")

# The common body plus the outputs of the current phase, so each leg can rewrite them without duplicate tables.
write_config() {
  {
    printf '%s\n' "$BASELINE"
    cat <<'COMMON'

[colors]
backdrop = "#000000FF"

[animation]
enabled = false

[animation.scratchpad]
enabled = false
dim = 0.0

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[[scratchpad]]
name = "pad"

[[scratchpad]]
name = "scaled"
proportional_size = true

[[window_rule]]
match.title = "^edge-member$"
default_output = "HEADLESS-1"
default_floating = true
default_floating_size_px = { width = 400, height = 240 }
default_position = { x = -300, y = 100, anchor = "top_left" }

[[window_rule]]
match.title = "^hanging-member$"
default_output = "HEADLESS-2"
default_floating = true
default_floating_size_px = { width = 400, height = 240 }
default_position = { x = -300, y = 200, anchor = "top_left" }
default_scratchpad = "pad"

[[window_rule]]
match.title = "^arriving-member$"
default_output = "HEADLESS-1"
default_floating = true
default_floating_size_px = { width = 400, height = 240 }
default_position = { x = 100, y = 100, anchor = "top_left" }

[[window_rule]]
match.title = "^scaled-member$"
default_output = "HEADLESS-2"
default_floating = true
default_floating_size_px = { width = 600, height = 400 }
default_scratchpad = "scaled"
COMMON
    printf '\n%s\n' "$1"
  } > "$UMBRIEL_CONFIG"
}

readonly TWO_OUTPUTS='
[output.HEADLESS-1]
enabled = true
mode = "1280x720"
position = [0, 0]

[output.HEADLESS-2]
enabled = true
mode = "1280x720"
position = [1280, 0]'
# Legacy 1280-based coordinates must not survive a replacement 800x600 output, and the replacement is not the connector
# the pad was displaced from, so the remap cannot come from the displaced restore.
readonly NARROW_REPLACEMENT='
[output.HEADLESS-1]
enabled = true
mode = "800x600"
position = [0, 0]'
# Leg E returns the pad to the connector it was displaced from in leg D, then takes that connector away again, so the
# clamp runs on a home that really is the one that went away.
readonly SMALL_PLUS='
[output.HEADLESS-2]
enabled = true
mode = "800x600"
position = [0, 0]

[output.HEADLESS-1]
enabled = true
mode = "800x600"
position = [800, 0]'
readonly WIDE_RETURN='
[output.HEADLESS-2]
enabled = true
mode = "1600x900"
position = [0, 0]

[output.HEADLESS-1]
enabled = true
mode = "1280x720"
position = [1600, 0]'

write_config "$TWO_OUTPUTS"
"$UMBRIEL" msg config-reload > /dev/null
# The pad actions resolve their output from the pointer, which starts outside the two-output layout.
"$POINTER" 2560 720 move 640 360
"$UMBRIEL" clock-freeze

# Leg A: a show must not reframe a member whose centre lies outside the pad's usable area.
spawn "$EDGE_TITLE"
wait_for_field "$EDGE_TITLE" x "$EDGE_X"
"$UMBRIEL" msg window-focus "$(field_of "$EDGE_TITLE" id)" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad "$PAD" > /dev/null
"$UMBRIEL" msg scratchpad-toggle "$PAD" > /dev/null
wait_for_scratchpad "$EDGE_TITLE" "$PAD"
if [[ $(field_of "$EDGE_TITLE" x) != "$EDGE_X" || $(field_of "$EDGE_TITLE" y) != "$EDGE_Y" ]]; then
  echo "showing the scratchpad moved its member: $(windows)"
  exit 1
fi
"$UMBRIEL" msg scratchpad-toggle "$PAD" > /dev/null
"$UMBRIEL" msg scratchpad-toggle "$PAD" > /dev/null
if [[ $(field_of "$EDGE_TITLE" x) != "$EDGE_X" || $(field_of "$EDGE_TITLE" y) != "$EDGE_Y" ]]; then
  echo "reopening the scratchpad moved its member: $(windows)"
  exit 1
fi
# The member is still grabbable, so the leg cannot pass by leaving a window lost off the output.
if (( $(visible_pixels "$SHOTS/shown.png" HEADLESS-1) < 5000 )); then
  echo "scratchpad member was not visible on its output after the reopen: $(windows)"
  exit 1
fi

# Leg B: the pad moving to the other output carries its member there, at that output's edge and not its centre.
"$UMBRIEL" msg scratchpad-toggle "$PAD" > /dev/null
"$POINTER" 2560 720 move 1920 360
"$UMBRIEL" msg scratchpad-toggle "$PAD" > /dev/null
wait_for_field "$EDGE_TITLE" output HEADLESS-2
wait_for_scratchpad "$EDGE_TITLE" "$PAD"
moved_x=$(field_of "$EDGE_TITLE" x)
moved_y=$(field_of "$EDGE_TITLE" y)
if ((moved_x != 1280 || moved_y != EDGE_Y)); then
  echo "the member did not land at the new output's edge: $moved_x,$moved_y"
  exit 1
fi
if (( $(visible_pixels "$SHOTS/moved.png" HEADLESS-2) < 5000 )); then
  echo "the member was not visible on the pad's new output: $(windows)"
  exit 1
fi

# Leg C: moving a window that sits on the first output into a pad already showing on the second joins the pad's output.
# The action addresses the window focused on the pointer's output, so the pointer returns to the first output while the
# pad stays visible on the second.
spawn "$ARRIVING_TITLE"
wait_for_field "$ARRIVING_TITLE" output HEADLESS-1
"$UMBRIEL" msg window-focus "$(field_of "$ARRIVING_TITLE" id)" > /dev/null
"$POINTER" 2560 720 move 640 360
"$UMBRIEL" msg window-move-to-scratchpad "$PAD" > /dev/null
wait_for_scratchpad "$ARRIVING_TITLE" "$PAD"
wait_for_field "$ARRIVING_TITLE" output HEADLESS-2
arriving_x=$(field_of "$ARRIVING_TITLE" x)
arriving_y=$(field_of "$ARRIVING_TITLE" y)
# 100,100 on the first output becomes 1280 + 100,100 on the second; a show-time centre would leave x at 1720.
if ((arriving_x != 1380 || arriving_y != 100)); then
  echo "the arriving member did not join the pad's output in its own basis: $arriving_x,$arriving_y"
  exit 1
fi

# Leg D: with every output gone, the members come back inside the output that replaces them. The replacement is a
# different connector from the one the pad was displaced from, so the placement has to come from moveScratchpad()'s own
# remap out of the area the pad was last sized against.
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
write_config "$NARROW_REPLACEMENT"
"$UMBRIEL" msg config-reload > /dev/null
if [[ $("$UMBRIEL" output-create HEADLESS-1 800x600) != HEADLESS-1 ]]; then
  echo "the replacement output could not be created"
  exit 1
fi
for title in "$ARRIVING_TITLE" "$EDGE_TITLE"; do
  wait_for_field "$title" output HEADLESS-1
  x=$(field_of "$title" x)
  y=$(field_of "$title" y)
  if ((x < 0 || x + WIDTH > 800 || y < 0 || y + HEIGHT > 600)); then
    echo "'$title' came back outside the replacement output at $x,$y: $(windows)"
    exit 1
  fi
done

# Leg E: a member hanging off an output that goes away comes back exactly at the grabbable margin of the wider output
# that replaces it, instead of proportionally off screen.
write_config "$SMALL_PLUS"
"$UMBRIEL" msg config-reload > /dev/null
if [[ $("$UMBRIEL" output-create HEADLESS-2 800x600) != HEADLESS-2 ]]; then
  echo "the home output could not be created"
  exit 1
fi
wait_for_field "$EDGE_TITLE" output HEADLESS-2
spawn "$HANGING_TITLE"
wait_for_field "$HANGING_TITLE" output HEADLESS-2
wait_for_field "$HANGING_TITLE" x "$EDGE_X"
wait_for_field "$HANGING_TITLE" y "$HANGING_Y"
wait_for_scratchpad "$HANGING_TITLE" "$PAD"
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
wait_for_field "$HANGING_TITLE" output HEADLESS-1
write_config "$WIDE_RETURN"
"$UMBRIEL" msg config-reload > /dev/null
if [[ $("$UMBRIEL" output-create HEADLESS-2 1600x900) != HEADLESS-2 ]]; then
  echo "the wider output could not be created"
  exit 1
fi
wait_for_field "$HANGING_TITLE" output HEADLESS-2
hanging_x=$(field_of "$HANGING_TITLE" x)
hanging_y=$(field_of "$HANGING_TITLE" y)
# -300/800 and 200/600 of the small output become -600,300 on 1600x900; the margin pulls x back to -325.
if ((hanging_x != -(WIDTH - MARGIN) || hanging_y != 300)); then
  echo "'$HANGING_TITLE' came back at $hanging_x,$hanging_y instead of the grabbable margin -$((WIDTH - MARGIN)),300"
  exit 1
fi

# Leg F: a window opening into a proportional pad that already shows on another output is sized once, against that
# output, and not scaled again by its admission.
"$POINTER" 2560 720 move 1920 360
"$UMBRIEL" msg scratchpad-toggle "$SCALED_PAD" > /dev/null
# The rule size is only observable in a client that maps at the size it was configured to.
spawn "$SCALED_TITLE" RESIZE_FILL_COLOR=0xFF5577AA
wait_for_scratchpad "$SCALED_TITLE" "$SCALED_PAD"
wait_for_field "$SCALED_TITLE" output HEADLESS-1
scaled_w=$(field_of "$SCALED_TITLE" w)
scaled_h=$(field_of "$SCALED_TITLE" h)
if ((scaled_w != SCALED_WIDTH || scaled_h != SCALED_HEIGHT)); then
  echo "'$SCALED_TITLE' opened at $scaled_w,$scaled_h instead of its rule size $SCALED_WIDTH,$SCALED_HEIGHT"
  exit 1
fi

echo "a show kept its member at $EDGE_X,$EDGE_Y, the pad moved it to $moved_x,$moved_y, an arrival joined at $arriving_x,$arriving_y, a narrow replacement kept both members, a hanging member came back at $hanging_x,$hanging_y, and a proportional pad kept its member at $scaled_w,$scaled_h"
