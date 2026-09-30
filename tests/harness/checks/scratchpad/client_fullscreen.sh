#!/usr/bin/env bash
# Verifies that client-requested fullscreen entry and exit on a scratchpad window
# (such as fullscreening a video in Discord/Equibop and closing it) keeps the window
# properly parented in the scratchpad scene tree above the backdrop dim rectangle,
# rather than leaving the window dimmed under the scratchpad backdrop.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/scratch-fullscreen-control"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/scratch-fullscreen.log"
readonly BEFORE="$UMBRIEL_RUNTIME_DIR/scratch-fullscreen-before.png"
readonly AFTER="$UMBRIEL_RUNTIME_DIR/scratch-fullscreen-after.png"

sample_window_center() {
  # Window is 400x300 centered on 1280x720: (640, 360)
  magick "$1" -crop 20x20+630+350 -format '%[fx:round(255*mean.r)]' info:
}

sample_corner() {
  # Top-left corner of output outside the scratchpad window: (20, 20)
  magick "$1" -crop 20x20+20+20 -format '%[fx:round(255*mean.r)]' info:
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#FFFFFFFF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[animation]
enabled = false

[animation.scratchpad]
dim = 0.6
scale = 0

[[scratchpad]]
name = "test"

[[window_rule]]
match.title = "^scratch-video$"
default_scratchpad = "test"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 440, y = 210, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"

FILL_COLOR=0xFFFF0000 FULLSCREEN_ON_STDIN=1 "$CLIENT" scratch-video 400 300 \
  <&"$control_fd" > "$CLIENT_LOG" 2>&1 &

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.05
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_count 1
"$UMBRIEL" msg "scratchpad-toggle:test" > /dev/null
"$UMBRIEL" settle

grim "$BEFORE"
initial_red=$(sample_window_center "$BEFORE")
initial_corner=$(sample_corner "$BEFORE")

# Request fullscreen via client protocol
printf f >&"$control_fd"
sleep 0.1
"$UMBRIEL" settle

# Request unfullscreen (simulating closing the fullscreen video in Discord)
printf u >&"$control_fd"
sleep 0.1
"$UMBRIEL" settle

grim "$AFTER"
after_red=$(sample_window_center "$AFTER")
after_corner=$(sample_corner "$AFTER")

# If the window dropped below the scratchpad dim rect, its red dropped from ~255 to ~102.
if (( after_red < 200 )); then
  echo "window remained dimmed after exiting fullscreen in scratchpad: red dropped from $initial_red to $after_red"
  exit 1
fi

echo "window remained bright in scratchpad after client fullscreen toggle: initial=$initial_red after=$after_red"
