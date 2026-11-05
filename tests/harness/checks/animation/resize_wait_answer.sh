#!/usr/bin/env bash
# windows_resize.wait_timeout_ms ends its hold when a resized client has drawn a new buffer, at the requested size or at
# a size of its own: a new window geometry over the old buffer keeps the hold, and a client that rounds the size to its
# own step releases it at once instead of at the timeout.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly TIMEOUT_MS=1000
readonly SHOTS="$UMBRIEL_RUNTIME_DIR/resize-answer"
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

readonly RED='r > 0.6 && g < 0.08 && b < 0.08'
readonly BLUE='b > 0.6 && r < 0.08 && g < 0.08'

expect() {
  local label=$1 shot=$2 color=$3 ex=$4 ew=$5 eh=${6:-720}
  local x y w h
  read -r x y w h <<< "$("$UMBRIEL_PIXEL_PROBE" "$shot" bbox "$color")"
  if ((x < ex - 2 || x > ex + 2 || w < ew - 2 || w > ew + 2 || h < eh - 2 || h > eh + 2)); then
    echo "$label: expected x=$ex w=$ew, got $x $y $w $h"
    exit 1
  fi
}

expect_no_blue() {
  local blue
  blue=$("$UMBRIEL_PIXEL_PROBE" "$2" count "$BLUE")
  if ((blue > 0)); then
    echo "$1: the opener showed before the switch ($blue pixels)"
    exit 1
  fi
}

# A lone red window is halved by a blue opener. Its client answers on `r`, under the given extra environment.
open_case() {
  local name=$1
  shift
  local fifo="$UMBRIEL_RUNTIME_DIR/$name-control" log="$UMBRIEL_RUNTIME_DIR/$name-lone.log"
  mkfifo "$fifo"
  exec {control_fd}<>"$fifo"
  env "$@" FILL_COLOR=0xFFFF0000 FOLLOW_CONFIGURES=1 RESIZE_ON_STDIN=1 \
    "$UMBRIEL_UNMAP_CLIENT" "$name-lone" 1280 720 < "$fifo" > "$log" 2>&1 &
  await_window "$name-lone"
  "$UMBRIEL" clock-advance 1500 > /dev/null
  FILL_COLOR=0xFF0000FF RESIZE_FILL_COLOR=0xFF0000FF \
    "$UMBRIEL_UNMAP_CLIENT" "$name-new" 1280 720 > "$UMBRIEL_RUNTIME_DIR/$name-new.log" 2>&1 &
  await_window "$name-new"
  await_lines "$log" 'resize-pending 640x720' 1 0.025
  "$UMBRIEL" clock-advance 1 > /dev/null
  "$UMBRIEL" clock-advance 50 > /dev/null
  grim "$SHOTS/$name-held.png"
  expect "$name: lone window before its client redrew" "$SHOTS/$name-held.png" "$RED" 0 1280
  expect_no_blue "$name: before the lone client redrew" "$SHOTS/$name-held.png"
  echo r >&"$control_fd"
  await_lines "$log" 'resized ' 1 0.025
  # Far inside the timeout, so only the client's answer can have released the hold.
  "$UMBRIEL" clock-advance 1 > /dev/null
  grim "$SHOTS/$name-switched.png"
}

# The client commits the halved window geometry over its full-width buffer first; that is not a redraw.
open_case geometry-ahead GEOMETRY_AHEAD=1
expect "geometry-ahead: lone window after its redraw" "$SHOTS/geometry-ahead-switched.png" "$RED" 0 640
expect "geometry-ahead: opener with the switch" "$SHOTS/geometry-ahead-switched.png" "$BLUE" 640 640

"$UMBRIEL" msg workspace-switch 2 > /dev/null
"$UMBRIEL" clock-advance 1500 > /dev/null
# The client answers 640 with 600, its own step; the switch lands with that answer, not at the timeout.
open_case rounded RESIZE_STEP=100
expect "rounded: lone window at its own size" "$SHOTS/rounded-switched.png" "$RED" 0 600 700
expect "rounded: opener with the switch" "$SHOTS/rounded-switched.png" "$BLUE" 640 640

echo "the hold ended at the client's real redraw, whether at the requested size or its own"
