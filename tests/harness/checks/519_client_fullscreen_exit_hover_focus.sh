#!/usr/bin/env bash
# A browser or media player leaves video fullscreen through an xdg-toplevel client request, not Umbriel's fullscreen
# action. The next motion inside a tile revealed beneath the stationary pointer must refresh hover focus.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/client-fullscreen-control"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/client-fullscreen.log"

windows() { "$UMBRIEL" windows --json; }

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.05
  done
  echo "expected $want windows, got: $(windows)"
  return 1
}

wait_for_query() {
  local query=$1 message=$2
  for _ in $(seq 60); do
    windows | jq -e "$query" > /dev/null && return 0
    sleep 0.05
  done
  echo "$message: $(windows)"
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

[layout]
mode = "scrolling"

[layout.scrolling]
default_extent_fraction = 0.5

[animation]
duration_ms = 1

[input.focus]
follows_mouse = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
FULLSCREEN_ON_STDIN=1 LOG_CONFIGURES=1 "$CLIENT" client-fullscreen-left 1200 700 \
  <&"$control_fd" > "$CLIENT_LOG" 2>&1 &
wait_for_count 1
"$CLIENT" client-fullscreen-right 1200 700 > /dev/null 2>&1 &
wait_for_count 2

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 322 360
left_id=$(windows | jq -r '.[] | select(.title == "client-fullscreen-left") | .id')
"$UMBRIEL" msg "window-focus:$left_id" > /dev/null
wait_for_query \
  '[.[] | select(.title == "client-fullscreen-left" and .active)] | length == 1' \
  "left tile did not take pointer focus"

printf f >&"$control_fd"
wait_for_configured_state fullscreen
wait_for_query \
  '[.[] | select(.title == "client-fullscreen-left" and .active)] | length == 1' \
  "client fullscreen request changed focus"

# The fullscreen client still owns this position. Once it leaves fullscreen,
# the right tile appears beneath the unchanged pointer without a geometric
# crossing between the next motion's old and new coordinates.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 1000 360
printf u >&"$control_fd"
wait_for_configured_state windowed
wait_for_query \
  '[.[] | select(.title == "client-fullscreen-left" and .active)] | length == 1' \
  "client unfullscreen request was not applied without changing focus"
sleep 0.1

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 1001 360
wait_for_query \
  '[.[] | select(.title == "client-fullscreen-right" and .active)] | length == 1' \
  "motion inside the tile revealed by a client fullscreen exit did not refresh focus"

echo "client fullscreen exit invalidates hover focus for the tile it reveals"
