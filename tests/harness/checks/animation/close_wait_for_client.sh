#!/usr/bin/env bash
# With windows_resize.wait_timeout_ms and windows_out disabled, a closed tile's last frame stays until the tile filling
# its space has redrawn at its new size. Then the closed frame goes and the survivor is at its target in the same frame,
# never stretched. A client that never redraws is released by the timeout.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly TIMEOUT_MS=300
readonly SHOTS="$UMBRIEL_RUNTIME_DIR/close-wait"
mkdir -p "$SHOTS"

cat >> "$UMBRIEL_CONFIG" <<EOF

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[layout]
mode = "dwindle"
gap = 0

[animation.windows_in]
enabled = false

[animation.windows_out]
enabled = false

[animation.windows_move]
enabled = true
duration_ms = 600
curve = "linear"

[animation.windows_resize]
enabled = false
wait_timeout_ms = $TIMEOUT_MS
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" clock-freeze

bounds() {
  local shot=$1 predicate=$2
  "$UMBRIEL_PIXEL_PROBE" "$shot" bbox "$predicate"
}
readonly RED='r > 0.6 && g < 0.08 && b < 0.08'
readonly BLUE='b > 0.6 && r < 0.08 && g < 0.08'

expect() {
  local label=$1 shot=$2 predicate=$3 ex=$4 ey=$5 ew=$6 eh=$7
  local x y w h
  read -r x y w h <<< "$(bounds "$shot" "$predicate")"
  if ((x < ex - 2 || x > ex + 2 || y < ey - 2 || y > ey + 2 || w < ew - 2 || w > ew + 2 || h < eh - 2 || h > eh + 2)); then
    echo "$label: expected $ex $ey $ew $eh, got $x $y $w $h"
    exit 1
  fi
}

expect_none() {
  local label=$1 shot=$2 predicate=$3 count
  count=$("$UMBRIEL_PIXEL_PROBE" "$shot" count "$predicate")
  if ((count > 0)); then
    echo "$label: $count pixels left"
    exit 1
  fi
}

# A blue closer on the left and a red survivor on the right that follows configures only when told to.
open_pair() {
  local name=$1 fifo=$2 log=$3
  FILL_COLOR=0xFF0000FF FOLLOW_CONFIGURES=1 "$UMBRIEL_UNMAP_CLIENT" "$name-closer" 1280 720 > "$UMBRIEL_RUNTIME_DIR/$name-closer.log" 2>&1 &
  await_window "$name-closer"
  "$UMBRIEL" clock-advance 1500 > /dev/null
  FILL_COLOR=0xFFFF0000 FOLLOW_CONFIGURES=1 RESIZE_ON_STDIN=1 \
    "$UMBRIEL_UNMAP_CLIENT" "$name-survivor" 1280 720 < "$fifo" > "$log" 2>&1 &
  await_window "$name-survivor"
  await_lines "$log" 'resize-pending 640x720' 1 0.025
  echo r > "$fifo"
  await_lines "$log" 'resized 640x720' 1 0.025
  "$UMBRIEL" clock-advance 1500 > /dev/null
  "$UMBRIEL" msg window-close "$(window_by_title "$name-closer" | jq -r .id)" > /dev/null
  await_lines "$log" 'resize-pending 1280x720' 1 0.025
}

readonly FIFO="$UMBRIEL_RUNTIME_DIR/answered-control"
readonly LOG="$UMBRIEL_RUNTIME_DIR/answered-survivor.log"
mkfifo "$FIFO"
exec {control_fd}<>"$FIFO"
open_pair answered "$FIFO" "$LOG"

# Until the survivor redraws, the screen is unchanged: the closed tile's frame and the survivor's unstretched frame.
"$UMBRIEL" clock-advance 1 > /dev/null
"$UMBRIEL" clock-advance $((TIMEOUT_MS / 2)) > /dev/null
grim "$SHOTS/held.png"
expect "closed tile while the survivor had not redrawn" "$SHOTS/held.png" "$BLUE" 0 0 640 720
expect "survivor while its client had not redrawn" "$SHOTS/held.png" "$RED" 640 0 640 720

# Its redraw switches both in one frame: the closed tile's frame goes and the survivor is at its target, redrawn.
echo r >&"$control_fd"
await_lines "$LOG" 'resized 1280x720' 1 0.025
"$UMBRIEL" clock-advance 1 > /dev/null
grim "$SHOTS/switched.png"
expect_none "closed tile once the survivor redrew" "$SHOTS/switched.png" "$BLUE"
expect "survivor once its client redrew" "$SHOTS/switched.png" "$RED" 0 0 1280 720

# A survivor that never redraws still lands, stretched, once the timeout passes.
"$UMBRIEL" msg workspace-switch 2 > /dev/null
"$UMBRIEL" clock-advance 1500 > /dev/null
readonly SILENT_FIFO="$UMBRIEL_RUNTIME_DIR/silent-control"
readonly SILENT_LOG="$UMBRIEL_RUNTIME_DIR/silent-survivor.log"
mkfifo "$SILENT_FIFO"
exec {silent_fd}<>"$SILENT_FIFO"
open_pair silent "$SILENT_FIFO" "$SILENT_LOG"
"$UMBRIEL" clock-advance 1 > /dev/null
"$UMBRIEL" clock-advance "$TIMEOUT_MS" > /dev/null
"$UMBRIEL" clock-advance 1 > /dev/null
"$UMBRIEL" clock-advance 1500 > /dev/null
grim "$SHOTS/timeout.png"
expect "silent survivor after the timeout" "$SHOTS/timeout.png" "$RED" 0 0 1280 720
expect_none "closed tile after the timeout" "$SHOTS/timeout.png" "$BLUE"

echo "the closed tile's frame stayed until the survivor redrew or the timeout passed, then both switched in one frame"
