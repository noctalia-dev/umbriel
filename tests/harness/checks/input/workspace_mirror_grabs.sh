#!/usr/bin/env bash
# harness: outputs=2
# Client-authorized move and resize grabs operate in source coordinates, not receiving pixels, and so does the edge a
# compositor resize picks from where the pointer is in the copy.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[animation]
enabled = false
[output.HEADLESS-1]
mode = "1280x720@60"
position = [0, 0]
workspaces = ["source", "away"]
[output.HEADLESS-2]
mode = "960x720@60"
position = [1280, 0]
workspaces = ["own", "source"]
[[workspace]]
output = "HEADLESS-2"
name = "source"
mirror = { output = "HEADLESS-1", name = "source" }
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[input]
mod_right_double_click = "none"
[[window_rule]]
match.title = "^mirror-grab$"
default_output = "HEADLESS-1"
default_workspace = "source"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 100, y = 100, anchor = "top_left" }
TOML
"$UMBRIEL" msg config-reload > /dev/null
# Starts the client in mode $1 (none for a compositor-driven grab) and shows its workspace only through the copy.
start_client() {
  "$UMBRIEL_SEAT_LOG_CLIENT" mirror-grab ${1:+"$1"} > "$UMBRIEL_RUNTIME_DIR/${1:-plain}.log" 2>&1 &
  client_pid=$!
  await_window mirror-grab
  "$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
  "$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
  "$UMBRIEL" settle > /dev/null
}
assert_window() {
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "mirror-grab")')
    if jq -e "$1" <<< "$window" > /dev/null; then return 0; fi
    sleep 0.025
  done
  echo "expected $1, got $window"
  exit 1
}
start_client move-on-press
mkfifo "$UMBRIEL_RUNTIME_DIR/move.control"
exec {move_fd}<> "$UMBRIEL_RUNTIME_DIR/move.control"
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 1445 240 press 272 hold move 1520 285 release 272 <&"$move_fd" &
pointer_pid=$!
await_grab "$UMBRIEL_RUNTIME_DIR/move-on-press.log"
printf '\n' >&"$move_fd"
wait "$pointer_pid"
"$UMBRIEL" settle > /dev/null
assert_window '.x == 200 and .y == 160 and .output == "HEADLESS-1"'
stop_client() {
  kill "$client_pid"
  wait "$client_pid" || true
  for _ in $(seq 80); do
    [[ $("$UMBRIEL" windows --json | jq length) == 0 ]] && break
    sleep 0.025
  done
}
stop_client
start_client resize-on-press
mkfifo "$UMBRIEL_RUNTIME_DIR/resize.control"
exec {resize_fd}<> "$UMBRIEL_RUNTIME_DIR/resize.control"
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 1445 240 press 272 hold move 1520 240 release 272 <&"$resize_fd" &
pointer_pid=$!
await_grab "$UMBRIEL_RUNTIME_DIR/resize-on-press.log"
printf '\n' >&"$resize_fd"
wait "$pointer_pid"
"$UMBRIEL" settle > /dev/null
assert_window '.w == 500 and .x == 100 and .output == "HEADLESS-1"'
"$UMBRIEL" workspaces --json | jq -e 'any(.[]; .output == "HEADLESS-1" and .name == "away" and .active)' > /dev/null
# Mod+right-drag from near the copy's left edge resizes the left edge: (1370,240) is 20 source pixels inside it.
stop_client
start_client
"$UMBRIEL_POINTER_CLIENT" 2240 720 mod logo move 1370 240 press 273 move 1445 240 release 273 mod none
"$UMBRIEL" settle > /dev/null
assert_window '.x == 200 and .w == 300 and .output == "HEADLESS-1"'
echo '75 receiving pixels move or resize the original by 100 logical pixels, on the edge picked in the copy.'
