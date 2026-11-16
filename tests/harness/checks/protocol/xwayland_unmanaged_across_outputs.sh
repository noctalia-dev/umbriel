#!/usr/bin/env bash
# harness: xwayland=true
# harness: outputs=3
# An override-redirect window that spans adjacent outputs, side by side or stacked, is shown where its client put it.
# One that also covers the gap of an L-shaped arrangement is moved inside the output holding its top-left corner.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

client_pid=
sequence=0

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[colors]
backdrop = "#00FF00FF"
[output.HEADLESS-1]
position = [0, 0]
[output.HEADLESS-2]
position = [1280, 0]
[output.HEADLESS-3]
position = [0, 720]
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

# Each "x y=r g b" pair names a pixel of a 1280x720 output and its expected color.
expect_pixels() {
  local output=$1 image="$UMBRIEL_RUNTIME_DIR/unmanaged-$1.png" point actual
  shift
  "$UMBRIEL" settle > /dev/null
  grim -o "$output" "$image"
  for point in "$@"; do
    actual=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel ${point%%=*})
    if [[ $actual != "${point#*=}" ]]; then
      echo "$output pixel ${point%%=*}: $actual, expected ${point#*=}"
      exit 1
    fi
  done
}

readonly WINDOW='48 96 144' BACKDROP='0 255 0'

# HEADLESS-2 is right of HEADLESS-1 and HEADLESS-3 below it; nothing is below HEADLESS-2.
# 120 px past HEADLESS-1's right edge, onto HEADLESS-2: shown in place across both.
start_client across
send 'override true' 'type notification' 'geometry 1100 300 300 160' map
await_lines "$client_log" mapped 1 0.025
expect_state 'map=2 override=true type=notification x=1100 y=300 w=300 h=160 .*'
expect_pixels HEADLESS-1 "1105 305=$WINDOW" "1279 455=$WINDOW" "1095 305=$BACKDROP"
expect_pixels HEADLESS-2 "0 305=$WINDOW" "115 455=$WINDOW" "125 305=$BACKDROP"

# 60 px past HEADLESS-1's bottom edge, onto HEADLESS-3: shown in place across both.
send 'geometry 500 620 300 160'
expect_state 'map=2 override=true type=notification x=500 y=620 w=300 h=160 .*'
expect_pixels HEADLESS-1 "505 625=$WINDOW" "795 719=$WINDOW" "505 615=$BACKDROP"
expect_pixels HEADLESS-3 "505 0=$WINDOW" "795 55=$WINDOW" "505 65=$BACKDROP"

# Over the corner where all three meet, its bottom-right part in the gap below HEADLESS-2: moved inside HEADLESS-1,
# to 980,560.
send 'geometry 1100 620 300 160'
expect_state 'map=2 override=true type=notification x=1100 y=620 w=300 h=160 .*'
expect_pixels HEADLESS-1 "985 565=$WINDOW" "1279 719=$WINDOW" "975 600=$BACKDROP"
expect_pixels HEADLESS-2 "5 700=$BACKDROP"
expect_pixels HEADLESS-3 "1105 5=$BACKDROP"
stop_client

echo 'X11 override-redirect windows spanning adjacent outputs stay in place; ones also over a gap move inside their output'
