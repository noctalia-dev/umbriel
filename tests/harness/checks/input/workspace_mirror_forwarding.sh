#!/usr/bin/env bash
# harness: outputs=2
# Real pointer and keyboard events reach the original client through a scaled copy without selecting the source, and a
# pointer lock taken there holds while the source output shows another workspace.
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
[[window_rule]]
match.title = "^mirror-input$"
default_output = "HEADLESS-1"
default_workspace = "source"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 100, y = 100, anchor = "top_left" }
TOML
"$UMBRIEL" msg config-reload > /dev/null
log=$UMBRIEL_RUNTIME_DIR/client.log
mkfifo "$UMBRIEL_RUNTIME_DIR/control"
exec {control_fd}<> "$UMBRIEL_RUNTIME_DIR/control"
LOG_POINTER_INPUT=1 POINTER_CONSTRAINT_CONTROL=1 "$UMBRIEL_SEAT_LOG_CLIENT" mirror-input <&"$control_fd" > "$log" 2>&1 &
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
control() {
  local n
  n=$(events "$log" "constraint-command $1")
  printf '%s' "$1" >&"$control_fd"
  await_events "$log" "constraint-command $1" "$((n + 1))"
}
position() {
  local n
  n=$(events "$log" 'pointer-position')
  printf p >&"$control_fd"
  await_events "$log" 'pointer-position' "$((n + 1))"
  sed -n 's/pointer-position //p' "$log" | tail -1
}
control l
await_events "$log" 'pointer-locked' 1
locked_at=$(position)
"$UMBRIEL_POINTER_CLIENT" 2240 720 relative 20 0
"$UMBRIEL" settle > /dev/null
if grep -q 'pointer-unlocked' "$log"; then
  echo "relative motion through the copy broke the pointer lock: $(grep -E 'pointer-(un)?locked' "$log" | tr '\n' '|')"
  exit 1
fi
if [[ $(position) != "$locked_at" ]]; then
  echo "the locked pointer moved from $locked_at to $(position)"
  exit 1
fi
control u
"$UMBRIEL" msg workspace-switch source --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 220 200 click 272 tap 46
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" 'press-position x=120 y=100' 2
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" 'keyboard-key code=46 state=pressed' 1
# Rule confinement keeps the pointer inside the window as the copy draws it, with the source hidden.
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[[window_rule]]
match.title = "^mirror-input$"
confine_pointer = true
TOML
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 1445 240 click 272
"$UMBRIEL" settle > /dev/null
leaves=$(events "$log" 'pointer-leave')
"$UMBRIEL_POINTER_CLIENT" 2240 720 relative 400 0
"$UMBRIEL" settle > /dev/null
if (($(events "$log" 'pointer-leave') != leaves)); then
  echo "a confined window lost the pointer through the copy: $(tail -3 "$log" | tr '\n' '|')"
  exit 1
fi
echo 'Scaled copy forwards input, keeps focus without selecting the source, and holds pointer locks and confinement.'
