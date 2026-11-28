#!/usr/bin/env bash
# harness: xwayland=true
# An override-redirect window that pokes out of its output only partly is shown inside it, while its X geometry stays
# what the client chose. One wholly off screen stays off screen. Steam places its toasts like the first one when its UI
# is scaled.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

client_pid=
sequence=0

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[colors]
backdrop = "#00FF00FF"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

send() { printf '%s\n' "$@" >&"$control_fd"; }

start_client() {
  local name=$1 fifo="$UMBRIEL_RUNTIME_DIR/unmanaged-$1.control"
  client_log="$UMBRIEL_RUNTIME_DIR/unmanaged-$name.log"
  mkfifo "$fifo"
  exec {control_fd}<> "$fifo"
  "$UMBRIEL_XWAYLAND_POPUP_CLIENT" "notificationtoasts_$name" <&"$control_fd" > "$client_log" 2>&1 &
  client_pid=$!
  await_lines "$client_log" ready 1 0.025
  send 'class steam'
}

stop_client() {
  send quit
  wait "$client_pid"
  client_pid=
  exec {control_fd}>&-
}

expect_state() {
  local pattern=$1 token
  for _ in $(seq 100); do
    sequence=$((sequence + 1))
    token="unmanaged_$sequence"
    send "state $token"
    await_lines "$client_log" "state label=$token " 1 0.01
    grep -qE "^state label=$token $pattern$" "$client_log" && return 0
    sleep 0.025
  done
  echo "window did not reach X11 state: $pattern"
  cat "$client_log"
  exit 1
}

# Each "x y=r g b" pair names a pixel of the 1280x720 output and its expected color.
expect_pixels() {
  local image="$UMBRIEL_RUNTIME_DIR/unmanaged.png" point actual
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$image"
  for point in "$@"; do
    actual=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel ${point%%=*})
    if [[ $actual != "${point#*=}" ]]; then
      echo "pixel ${point%%=*}: $actual, expected ${point#*=}"
      exit 1
    fi
  done
}

readonly WINDOW='48 96 144' BACKDROP='0 255 0'

# 100 px past the right edge and 60 px past the bottom: shown at 980,560 instead.
start_client corner
send 'override true' 'type notification' 'geometry 1080 620 300 160' map
await_lines "$client_log" mapped 1 0.025
expect_state 'map=2 override=true type=notification x=1080 y=620 w=300 h=160 .*'
expect_pixels "985 565=$WINDOW" "1279 719=$WINDOW" "975 600=$BACKDROP" "1100 555=$BACKDROP"

# Sliding further out keeps it at the edge; moving fully inside follows the client again.
send 'geometry 1100 650 300 160'
expect_state 'map=2 override=true type=notification x=1100 y=650 w=300 h=160 .*'
expect_pixels "985 565=$WINDOW" "975 600=$BACKDROP"
send 'geometry 900 500 300 160'
expect_state 'map=2 override=true type=notification x=900 y=500 w=300 h=160 .*'
expect_pixels "905 505=$WINDOW" "1195 655=$WINDOW" "1205 600=$BACKDROP" "1279 719=$BACKDROP"

# Wholly below the output: nothing of it is shown.
send 'geometry 900 800 300 160'
expect_state 'map=2 override=true type=notification x=900 y=800 w=300 h=160 .*'
expect_pixels "1050 715=$BACKDROP" "1279 719=$BACKDROP" "905 505=$BACKDROP"
stop_client

echo 'X11 override-redirect windows partly off their output are shown inside it; wholly off-screen ones stay hidden'
