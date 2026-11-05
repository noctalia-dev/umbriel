#!/usr/bin/env bash
# With windows_resize.wait_timeout_ms, a Mod+right-drag resize of a tile steps through held switches: each step keeps
# showing the last frames until the resized client has redrawn, then lands in one frame, so no frame ever shows a buffer
# stretched or squeezed to a box it was not drawn for. Pointer motion during a held step is applied once it lands.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_RIGHT=273
readonly TIMEOUT_MS=200
readonly SHOTS="$UMBRIEL_RUNTIME_DIR/resize-drag"
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
mode = "scrolling"
gap = 0

[layout.scrolling]
default_extent_fraction = 0.5

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
  "$UMBRIEL_PIXEL_PROBE" "$1" bbox "$2"
}
readonly RED='r > 0.6 && g < 0.08 && b < 0.08'
readonly BLUE='b > 0.6 && r < 0.08 && g < 0.08'

expect() {
  local label=$1 shot=$2 color=$3 ex=$4 ew=$5
  local x y w h
  read -r x y w h <<< "$(bounds "$shot" "$color")"
  if ((x < ex - 2 || x > ex + 2 || w < ew - 2 || w > ew + 2 || h < 718)); then
    echo "$label: expected x=$ex w=$ew, got $x $y $w $h"
    exit 1
  fi
}

fifo="$UMBRIEL_RUNTIME_DIR/red-control"
mkfifo "$fifo"
exec {red_fd}<>"$fifo"
log="$UMBRIEL_RUNTIME_DIR/red.log"
FILL_COLOR=0xFFFF0000 FOLLOW_CONFIGURES=1 RESIZE_ON_STDIN=1 \
  "$UMBRIEL_UNMAP_CLIENT" red 1280 720 < "$fifo" > "$log" 2>&1 &
await_window red
"$UMBRIEL" clock-advance 1500 > /dev/null
FILL_COLOR=0xFF0000FF RESIZE_FILL_COLOR=0xFF0000FF \
  "$UMBRIEL_UNMAP_CLIENT" blue 1280 720 > "$UMBRIEL_RUNTIME_DIR/blue.log" 2>&1 &
await_window blue
await_lines "$log" 'resize-pending 640x720' 1 0.025
echo r >&"$red_fd"
await_lines "$log" 'resized 640x720' 1 0.025
"$UMBRIEL" clock-advance 1500 > /dev/null
"$UMBRIEL" clock-advance 1500 > /dev/null
grim "$SHOTS/before.png"
expect "red before the drag" "$SHOTS/before.png" "$RED" 0 640
expect "blue before the drag" "$SHOTS/before.png" "$BLUE" 640 640

# Each step is held on the pointer helper, so a screenshot shows the drag between motions.
pointer_hold 1280 720 move 620 360 mod logo press "$BTN_RIGHT" move 440 360 \
  -- move 340 360 mark step2 hold release "$BTN_RIGHT" mod none

# First step: red is asked for 460 but has not redrawn, so the screen still shows the last layout.
"$UMBRIEL" clock-advance 1 > /dev/null
await_lines "$log" 'resize-pending 460x720' 1 0.025
"$UMBRIEL" clock-advance $((TIMEOUT_MS / 2)) > /dev/null
grim "$SHOTS/step1-held.png"
expect "red while its client had not redrawn at 460" "$SHOTS/step1-held.png" "$RED" 0 640
expect "blue while red had not redrawn at 460" "$SHOTS/step1-held.png" "$BLUE" 640 640

# The pointer moves on while the step is held; that motion waits for the switch.
pointer_step step2
"$UMBRIEL" clock-advance 1 > /dev/null
grim "$SHOTS/step2-deferred.png"
expect "red while a later motion waited" "$SHOTS/step2-deferred.png" "$RED" 0 640

echo r >&"$red_fd"
await_lines "$log" 'resized 460x720' 1 0.025
"$UMBRIEL" clock-advance 1 > /dev/null
grim "$SHOTS/step1-switched.png"
expect "red once its client redrew at 460" "$SHOTS/step1-switched.png" "$RED" 0 460
expect "blue with red's switch to 460" "$SHOTS/step1-switched.png" "$BLUE" 460 640

# The deferred motion is the next step, held the same way.
"$UMBRIEL" clock-advance 1 > /dev/null
await_lines "$log" 'resize-pending 360x720' 1 0.025
"$UMBRIEL" clock-advance 1 > /dev/null
grim "$SHOTS/step2-held.png"
expect "red while its client had not redrawn at 360" "$SHOTS/step2-held.png" "$RED" 0 460
expect "blue while red had not redrawn at 360" "$SHOTS/step2-held.png" "$BLUE" 460 640

echo r >&"$red_fd"
await_lines "$log" 'resized 360x720' 1 0.025
"$UMBRIEL" clock-advance 1 > /dev/null
grim "$SHOTS/step2-switched.png"
expect "red once its client redrew at 360" "$SHOTS/step2-switched.png" "$RED" 0 360
expect "blue with red's switch to 360" "$SHOTS/step2-switched.png" "$BLUE" 360 640

pointer_release
"$UMBRIEL" clock-advance 1500 > /dev/null
grim "$SHOTS/after.png"
# Releasing lets the scrolling layout centre the narrower strip, a move that resizes nothing.
read -r rx _ rw _ <<< "$(bounds "$SHOTS/after.png" "$RED")"
expect "red after the drag" "$SHOTS/after.png" "$RED" "$rx" 360
expect "blue after the drag" "$SHOTS/after.png" "$BLUE" $((rx + 360)) 640

echo "a tiled drag resize held each step until the client redrew, then switched in one frame without stretching"
