#!/usr/bin/env bash
# windows_resize.wait_timeout_ms holds a reflow that resizes windows at its starting frames until each resized client
# has redrawn at its new size, then switches to the final layout in one frame. A client that never redraws releases
# the hold once wait_timeout_ms passes.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly MOVE_MS=600
readonly TIMEOUT_MS=200
readonly SHOTS="$UMBRIEL_RUNTIME_DIR/resize-wait"
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
duration_ms = $MOVE_MS
curve = "linear"

[animation.windows_resize]
enabled = false
wait_timeout_ms = $TIMEOUT_MS
EOF
"$UMBRIEL" msg config-reload > /dev/null
# Animation time only moves by clock-advance; clients still configure and commit in real time.
"$UMBRIEL" clock-freeze

red_bounds() {
  "$UMBRIEL_PIXEL_PROBE" "$1" bbox 'r > 0.6 && g < 0.08 && b < 0.08'
}

expect_bounds() {
  local label=$1 shot=$2 tolerance=$3 ex=$4 ey=$5 ew=$6 eh=$7
  local x y w h
  read -r x y w h <<< "$(red_bounds "$shot")"
  if ! ((x >= ex - tolerance && x <= ex + tolerance \
    && y >= ey - tolerance && y <= ey + tolerance \
    && w >= ew - tolerance && w <= ew + tolerance \
    && h >= eh - tolerance && h <= eh + tolerance)); then
    echo "$label: expected $ex $ey $ew $eh, got $x $y $w $h"
    exit 1
  fi
}

# The left window closes, so the red survivor both moves from the right half to x=0 and grows to the full width. It
# follows configures only when told to on its control fifo.
run_case() {
  local name=$1 answer=$2
  local fifo="$UMBRIEL_RUNTIME_DIR/$name-control" log="$UMBRIEL_RUNTIME_DIR/$name-survivor.log"
  mkfifo "$fifo"
  local control_fd
  exec {control_fd}<>"$fifo"

  "$UMBRIEL_UNMAP_CLIENT" "$name-closer" 1280 720 > "$UMBRIEL_RUNTIME_DIR/$name-closer.log" 2>&1 &
  await_window "$name-closer"
  "$UMBRIEL" clock-advance 1500 > /dev/null
  FILL_COLOR=0xFFFF0000 FOLLOW_CONFIGURES=1 RESIZE_ON_STDIN=1 \
    "$UMBRIEL_UNMAP_CLIENT" "$name-survivor" 1280 720 < "$fifo" > "$log" 2>&1 &
  await_window "$name-survivor"
  # The survivor answers its half-width configure at once, so the hold starts from an up-to-date frame.
  await_lines "$log" 'resize-pending 640x720' 1 0.025
  echo r >&"$control_fd"
  await_lines "$log" 'resized 640x720' 1 0.025
  "$UMBRIEL" clock-advance 1500 > /dev/null
  grim "$SHOTS/$name-before.png"
  expect_bounds "$name survivor before the close" "$SHOTS/$name-before.png" 2 640 0 640 720

  "$UMBRIEL" msg window-close "$(window_by_title "$name-closer" | jq -r .id)" > /dev/null
  await_lines "$log" 'resize-pending 1280x720' 1 0.025

  if [[ $answer == answer ]]; then
    # Far past windows_move's own length, the unanswered survivor is still at its starting box and frame.
    "$UMBRIEL" clock-advance $((TIMEOUT_MS / 2)) > /dev/null
    grim "$SHOTS/$name-held.png"
    expect_bounds "$name survivor while its client had not redrawn" "$SHOTS/$name-held.png" 2 640 0 640 720

    echo r >&"$control_fd"
    await_lines "$log" 'resized 1280x720' 1 0.025
    # The next tick switches at once: the survivor is at its target with its own new frame, without moving there.
    "$UMBRIEL" clock-advance 1 > /dev/null
    grim "$SHOTS/$name-switched.png"
    expect_bounds "$name survivor once its client redrew" "$SHOTS/$name-switched.png" 2 0 0 1280 720
  else
    "$UMBRIEL" clock-advance $((TIMEOUT_MS / 2)) > /dev/null
    grim "$SHOTS/$name-held.png"
    expect_bounds "$name survivor before the timeout" "$SHOTS/$name-held.png" 2 640 0 640 720
    # Past the timeout the reflow switches without the client, which never redraws.
    "$UMBRIEL" clock-advance "$TIMEOUT_MS" > /dev/null
    "$UMBRIEL" clock-advance 1 > /dev/null
    grim "$SHOTS/$name-switched.png"
    expect_bounds "$name survivor after the timeout" "$SHOTS/$name-switched.png" 2 0 0 1280 720
  fi
}

# Opening a second window resizes the lone one. Until that client has redrawn at half width, the screen keeps showing
# the lone window's frozen full-width frame and none of the opener; then both appear in their final boxes at once.
open_case() {
  local fifo="$UMBRIEL_RUNTIME_DIR/open-control" log="$UMBRIEL_RUNTIME_DIR/open-lone.log"
  mkfifo "$fifo"
  local control_fd
  exec {control_fd}<>"$fifo"
  FILL_COLOR=0xFFFF0000 FOLLOW_CONFIGURES=1 RESIZE_ON_STDIN=1 \
    "$UMBRIEL_UNMAP_CLIENT" open-lone 1280 720 < "$fifo" > "$log" 2>&1 &
  await_window open-lone
  "$UMBRIEL" clock-advance 1500 > /dev/null
  grim "$SHOTS/open-before.png"
  expect_bounds "lone window before the open" "$SHOTS/open-before.png" 2 0 0 1280 720

  FILL_COLOR=0xFF0000FF RESIZE_FILL_COLOR=0xFF0000FF \
    "$UMBRIEL_UNMAP_CLIENT" open-new 1280 720 > "$UMBRIEL_RUNTIME_DIR/open-new.log" 2>&1 &
  await_window open-new
  await_lines "$log" 'resize-pending 640x720' 1 0.025
  "$UMBRIEL" clock-advance 1 > /dev/null
  "$UMBRIEL" clock-advance $((TIMEOUT_MS / 2)) > /dev/null
  grim "$SHOTS/open-held.png"
  expect_bounds "lone window while its client had not redrawn" "$SHOTS/open-held.png" 2 0 0 1280 720
  local blue
  blue=$("$UMBRIEL_PIXEL_PROBE" "$SHOTS/open-held.png" count 'b > 0.6 && r < 0.08 && g < 0.08')
  if ((blue > 0)); then
    echo "the opener showed before the lone window had redrawn: $blue pixels"
    exit 1
  fi

  echo r >&"$control_fd"
  await_lines "$log" 'resized 640x720' 1 0.025
  "$UMBRIEL" clock-advance 1 > /dev/null
  grim "$SHOTS/open-switched.png"
  expect_bounds "lone window after the switch" "$SHOTS/open-switched.png" 2 0 0 640 720
  local x y w h
  read -r x y w h <<< "$("$UMBRIEL_PIXEL_PROBE" "$SHOTS/open-switched.png" bbox 'b > 0.6 && r < 0.08 && g < 0.08')"
  if ((x < 638 || x > 642 || w < 638 || w > 642 || h < 718)); then
    echo "the opener did not appear in its final box with the switch: got $x $y $w $h"
    exit 1
  fi
}

run_case resize-wait-answered answer
"$UMBRIEL" msg workspace-switch 2 > /dev/null
"$UMBRIEL" clock-advance 1500 > /dev/null
run_case resize-wait-timeout silent
"$UMBRIEL" msg workspace-switch 3 > /dev/null
"$UMBRIEL" clock-advance 1500 > /dev/null
open_case

echo "resizing reflows held their frozen frames and hidden openers until the clients redrew or the timeout passed, then switched in one frame"
