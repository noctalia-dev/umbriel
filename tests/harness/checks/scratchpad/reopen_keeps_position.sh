#!/usr/bin/env bash
# harness: outputs=2
# Showing a scratchpad leaves its members where they are. A window that hangs off an edge but keeps its grabbable
# margin is untouched, however far its centre lies outside the pad's usable area, and a reopen after a hide is
# identical to the first show. A pad whose output comes back at a new layout position lands its members on that output
# with their output-relative position kept, not at its centre.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly TITLE=reopen-member
readonly SHOTS="$UMBRIEL_RUNTIME_DIR/reopen-keeps-position"
# 400x240 at x = -300 leaves 100 px visible, inside the 75 px float margin, with its centre 100 px off the output.
readonly EDGE_X=-300
readonly EDGE_Y=100
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

cat >> "$UMBRIEL_CONFIG" <<'EOF'

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

[output.HEADLESS-1]
enabled = true
mode = "1280x720"
position = [0, 0]

[output.HEADLESS-2]
enabled = true
mode = "1280x720"
position = [1280, 0]

[[window_rule]]
match.title = "^reopen-"
default_output = "HEADLESS-1"
default_floating = true
default_floating_size_px = { width = 400, height = 240 }
default_position = { x = -300, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
# The pad actions resolve their output from the pointer, which starts outside the two-output layout.
"$POINTER" 2560 720 move 640 360
"$UMBRIEL" clock-freeze

spawn() {
  FILL_COLOR=0xFF5577AA "$CLIENT" "$TITLE" 400 240 > "$UMBRIEL_RUNTIME_DIR/$TITLE.log" 2>&1 &
  for _ in $(seq 100); do
    grep -q '^mapped$' "$UMBRIEL_RUNTIME_DIR/$TITLE.log" && return 0
    sleep 0.025
  done
  echo "client never mapped: $(cat "$UMBRIEL_RUNTIME_DIR/$TITLE.log")"
  exit 1
}

spawn
wait_for_field "$TITLE" x "$EDGE_X"
wait_for_field "$TITLE" y "$EDGE_Y"

# Leg A: the report — a show must not reframe a member whose centre is outside the usable area.
"$UMBRIEL" msg window-focus "$(field_of "$TITLE" id)" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
"$UMBRIEL" msg scratchpad-toggle > /dev/null
wait_for_field "$TITLE" scratchpad default
if [[ $(field_of "$TITLE" x) != "$EDGE_X" || $(field_of "$TITLE" y) != "$EDGE_Y" ]]; then
  echo "showing the scratchpad moved its member: $(windows)"
  exit 1
fi
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" msg scratchpad-toggle > /dev/null
if [[ $(field_of "$TITLE" x) != "$EDGE_X" || $(field_of "$TITLE" y) != "$EDGE_Y" ]]; then
  echo "reopening the scratchpad moved its member: $(windows)"
  exit 1
fi

# The member is still grabbable: its visible strip is on the output, so a check cannot pass by leaving it lost.
grim "$SHOTS/shown.png"
visible_pixels=$("$UMBRIEL_PIXEL_PROBE" "$SHOTS/shown.png" count 'r < 0.45 && g > 0.28 && b > 0.55')
if ((visible_pixels < 5000)); then
  echo "scratchpad member was not visible on the output after its reopen: pixels=$visible_pixels"
  exit 1
fi

# Leg B: a pad shown on another output carries its member there with its output-relative position, and does not park it
# at that output's centre either.
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$POINTER" 2560 720 move 1920 360
"$UMBRIEL" msg scratchpad-toggle > /dev/null
wait_for_field "$TITLE" output HEADLESS-2
wait_for_field "$TITLE" scratchpad default
moved_x=$(field_of "$TITLE" x)
moved_y=$(field_of "$TITLE" y)
if ((moved_x < 1280)); then
  echo "the member did not land on the pad's new output: $moved_x,$moved_y"
  exit 1
fi
centre_x=$((1280 + (1280 - 400) / 2))
centre_y=$(((720 - 240) / 2))
if ((moved_x == centre_x && moved_y == centre_y)); then
  echo "the member was centred on the pad's new output instead of keeping its place: $moved_x,$moved_y"
  exit 1
fi
grim -o HEADLESS-2 "$SHOTS/moved.png"
moved_pixels=$("$UMBRIEL_PIXEL_PROBE" "$SHOTS/moved.png" count 'r < 0.45 && g > 0.28 && b > 0.55')
if ((moved_pixels < 5000)); then
  echo "the member was not visible on the pad's new output: pixels=$moved_pixels"
  exit 1
fi

echo "scratchpad shows kept their member at $EDGE_X,$EDGE_Y and a pad moved to another output placed it at $moved_x,$moved_y"
