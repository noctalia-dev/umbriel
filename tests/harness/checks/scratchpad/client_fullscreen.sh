#!/usr/bin/env bash
# A scratchpad window that leaves client-requested fullscreen (a video player closing its fullscreen view) returns to
# the scratchpad tree, above the scratchpad's backdrop dim, instead of dropping beneath it.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/scratch-fullscreen-control"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/scratch-fullscreen.log"
readonly BEFORE="$UMBRIEL_RUNTIME_DIR/scratch-fullscreen-before.png"
readonly AFTER="$UMBRIEL_RUNTIME_DIR/scratch-fullscreen-after.png"

# The 400x300 window is centered on the 1280x720 output.
window_center_red() {
  local red
  read -r red _ _ < <("$UMBRIEL_PIXEL_PROBE" "$1" pixel 640 360)
  echo "$red"
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.05
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_configured_state() {
  local want=$1 configured=
  for _ in $(seq 60); do
    configured=$(grep '^configured-state=' "$CLIENT_LOG" | tail -1 || true)
    [[ $configured == *" $want" ]] && return 0
    sleep 0.05
  done
  echo "expected latest client configure to be $want, got '${configured:-none}': $(cat "$CLIENT_LOG")"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

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
FILL_COLOR=0xFFFF0000 FULLSCREEN_ON_STDIN=1 LOG_CONFIGURES=1 "$CLIENT" scratch-video 400 300 \
  <&"$control_fd" > "$CLIENT_LOG" 2>&1 &

wait_for_count 1
"$UMBRIEL" msg "scratchpad-toggle:test" > /dev/null
"$UMBRIEL" settle

grim "$BEFORE"
initial_red=$(window_center_red "$BEFORE")
if (( initial_red < 200 )); then
  echo "shown scratchpad window was dimmed before fullscreen: red=$initial_red"
  exit 1
fi

printf f >&"$control_fd"
wait_for_configured_state fullscreen
printf u >&"$control_fd"
wait_for_configured_state windowed
"$UMBRIEL" settle

grim "$AFTER"
after_red=$(window_center_red "$AFTER")
# Beneath the 0.6 dim, the red window reads about 102.
if (( after_red < 200 )); then
  echo "window remained dimmed after exiting fullscreen in scratchpad: red dropped from $initial_red to $after_red"
  exit 1
fi

echo "scratchpad window stays above its backdrop dim after a client fullscreen round trip"
