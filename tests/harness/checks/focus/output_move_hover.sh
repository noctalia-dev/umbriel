#!/usr/bin/env bash
# harness: outputs=2
# Output actions warp the pointer to the target output's center. With follows_mouse, landing on another window there
# must not take keyboard focus from the window the action moved or focused.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

windows() { "$UMBRIEL" windows --json; }
window_id() { windows | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'; }
window_center() { windows | jq -r --arg title "$1" '.[] | select(.title == $title) | "\(.x + .w / 2 | floor) \(.y + .h / 2 | floor)"'; }

# The focused workspace is the active one on the pointer's output, so this proves where the warp put the pointer.
expect_pointer_output() {
  "$UMBRIEL" workspaces --json | jq -e --arg output "$1" 'any(.[]; .focused and .output == $output)' > /dev/null \
    || { echo "expected the pointer on $1: $("$UMBRIEL" workspaces --json)"; exit 1; }
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.05
  done
  echo "expected $want windows, got: $(windows)"
  return 1
}

wait_for_active() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end') == "$want" ]] && return 0
    sleep 0.05
  done
  echo "expected '$want' to be active: $(windows)"
  return 1
}

# The cover window must sit over the destination's center, or the check would pass without the pointer ever landing on
# a competing window.
expect_cover_at_center() {
  local output=$1 center_x=$2 center_y=$3
  windows | jq -e --arg output "$output" --argjson cx "$center_x" --argjson cy "$center_y" '
    any(.[]; .title == "output-move-cover" and .output == $output
      and .x < $cx and .x + .w > $cx and .y < $cy and .y + .h > $cy)' > /dev/null \
    || { echo "output-move-cover does not cover the center of $output: $(windows)"; exit 1; }
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[input.focus]
follows_mouse = true

[output.HEADLESS-1]
position = [0, 0]

[output.HEADLESS-2]
position = [1280, 0]

[[window_rule]]
match.title = "^output-move-tiled$"
default_output = "HEADLESS-1"

[[window_rule]]
match.title = "^output-move-cover$"
default_output = "HEADLESS-2"
default_floating = true
default_floating_size_px = { width = 600, height = 400 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" output-move-tiled 600 400 > "$UMBRIEL_RUNTIME_DIR/output-move-tiled.log" 2>&1 &
wait_for_count 1
"$CLIENT" output-move-cover 600 400 > "$UMBRIEL_RUNTIME_DIR/output-move-cover.log" 2>&1 &
wait_for_count 2
"$UMBRIEL" settle

tiled_id=$(window_id output-move-tiled)
expect_cover_at_center HEADLESS-2 1920 360

# Start with the pointer resting on the tiled window, so hover focus agrees with keyboard focus before the move.
read -r tiled_x tiled_y <<< "$(window_center output-move-tiled)"
"$POINTER" 2560 720 move "$tiled_x" "$tiled_y"
"$UMBRIEL" msg window-focus "$tiled_id" > /dev/null
wait_for_active output-move-tiled

# The window travels to HEADLESS-2 and the pointer follows to its center, which is on the floating cover window.
"$UMBRIEL" msg window-move-to-output-right > /dev/null
"$UMBRIEL" settle
windows | jq -e '.[] | select(.title == "output-move-tiled") | .output == "HEADLESS-2"' > /dev/null \
  || { echo "window-move-to-output-right did not move output-move-tiled: $(windows)"; exit 1; }
expect_pointer_output HEADLESS-2
wait_for_active output-move-tiled

# Focusing the output again lands on the cover the same way and restores the window last focused there.
"$UMBRIEL" msg output-focus-left > /dev/null
"$UMBRIEL" settle
expect_pointer_output HEADLESS-1
"$UMBRIEL" msg output-focus-right > /dev/null
"$UMBRIEL" settle
expect_pointer_output HEADLESS-2
wait_for_active output-move-tiled

# The whole workspace, cover included, travels back to HEADLESS-1 and the pointer lands on the cover again there.
"$UMBRIEL" msg workspace-move-to-output-left > /dev/null
"$UMBRIEL" settle
windows | jq -e 'all(.[]; .output == "HEADLESS-1")' > /dev/null \
  || { echo "workspace-move-to-output-left did not move the workspace: $(windows)"; exit 1; }
expect_cover_at_center HEADLESS-1 640 360
expect_pointer_output HEADLESS-1
wait_for_active output-move-tiled

# The pointer really is over the cover: real motion there hands it focus, as follows_mouse should.
"$POINTER" 2560 720 move 641 361
wait_for_active output-move-cover

echo "window moves, output focus, and workspace moves kept keyboard focus under follows_mouse"
