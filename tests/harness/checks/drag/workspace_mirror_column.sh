#!/usr/bin/env bash
# harness: outputs=2
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
workspaces = ["own", "source@HEADLESS-1"]
[layout.scrolling]
default_extent_fraction = 0.4
[[window_rule]]
match.title = "^mirror-tile-"
default_output = "HEADLESS-1"
default_workspace = "source"
TOML
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL_SEAT_LOG_CLIENT" mirror-tile-a move-on-press > "$UMBRIEL_RUNTIME_DIR/a.log" 2>&1 &
await_window mirror-tile-a
"$UMBRIEL_SEAT_LOG_CLIENT" mirror-tile-b > "$UMBRIEL_RUNTIME_DIR/b.log" 2>&1 &
await_window mirror-tile-b
"$UMBRIEL" msg workspace-switch source --output HEADLESS-1 > /dev/null
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
"$UMBRIEL" settle > /dev/null
windows=$("$UMBRIEL" windows --json)
read -r ax ay aw ah < <(jq -r '.[] | select(.title == "mirror-tile-a") | "\(.x) \(.y) \(.w) \(.h)"' <<< "$windows")
read -r bx by bw bh < <(jq -r '.[] | select(.title == "mirror-tile-b") | "\(.x) \(.y) \(.w) \(.h)"' <<< "$windows")
[[ $(jq -r '.[] | select(.title == "mirror-tile-a").column' <<< "$windows") == 0 ]]
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
start_x=$((1280 + (ax + aw / 2) * 3 / 4))
start_y=$((90 + (ay + ah / 2) * 3 / 4))
end_x=$((1280 + (bx + bw - 20) * 3 / 4))
mkfifo "$UMBRIEL_RUNTIME_DIR/control"
exec {control_fd}<> "$UMBRIEL_RUNTIME_DIR/control"
"$UMBRIEL_POINTER_CLIENT" 2240 720 move "$start_x" "$start_y" press 272 hold move "$end_x" "$start_y" release 272 <&"$control_fd" &
pointer_pid=$!
await_grab "$UMBRIEL_RUNTIME_DIR/a.log"
printf '\n' >&"$control_fd"
wait "$pointer_pid"
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" windows --json | jq -e '
  any(.[]; .title == "mirror-tile-a" and .column == 1 and .output == "HEADLESS-1")
  and any(.[]; .title == "mirror-tile-b" and .column == 0 and .output == "HEADLESS-1")' > /dev/null
"$UMBRIEL" workspaces --json | jq -e 'any(.[]; .output == "HEADLESS-1" and .name == "away" and .active)' > /dev/null
echo 'Dragging a column through its scaled copy reorders the original without selecting or moving its workspace.'
