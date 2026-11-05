#!/usr/bin/env bash
# harness: outputs=2
# Real pointer and keyboard events reach the original client through a scaled copy without selecting the source.
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
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[[window_rule]]
match.title = "^mirror-input$"
default_output = "HEADLESS-1"
default_workspace = "source"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 100, y = 100, anchor = "top_left" }
TOML
"$UMBRIEL" msg config-reload > /dev/null
LOG_POINTER_INPUT=1 "$UMBRIEL_SEAT_LOG_CLIENT" mirror-input > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 60); do
  count=$("$UMBRIEL" windows --json | jq '[.[] | select(.title == "mirror-input")] | length')
  [[ $count == 1 ]] && break
  sleep 0.025
done
[[ $count == 1 ]]
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 1445 240 click 272 notch 1 tap 30
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" pointer-enter 1
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" 'press-position x=120 y=100' 1
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" 'pointer-axis axis=0 value=15.000' 1
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" 'keyboard-key code=30 state=pressed' 1
"$UMBRIEL" workspaces --json | jq -e '
  any(.[]; .output == "HEADLESS-1" and .name == "away" and .active)
  and any(.[]; .output == "HEADLESS-2" and .name == "source" and .active and .mirror)' > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL_POINTER_CLIENT" 2240 720 tap 48
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" 'keyboard-key code=48 state=pressed' 1
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 1445 240 press 272 move 1520 240 release 272
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" 'pointer-motion x=220.000 y=100.000' 1
"$UMBRIEL" msg workspace-switch source --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 220 200 click 272 tap 46
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" 'press-position x=120 y=100' 2
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" 'keyboard-key code=46 state=pressed' 1
echo 'Scaled copy forwards surface coordinates, clicks, wheel and retained keyboard focus without selecting the source.'
