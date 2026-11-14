#!/usr/bin/env bash
# Native data-drag destinations receive held and changing modifiers without keyboard focus.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = false
[[window_rule]]
match.title = "^modifier-keyboard-owner$"
default_floating = true
default_position = { x = 200, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^modifier-drop-target$"
default_floating = true
default_position = { x = 800, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
owner_log="$UMBRIEL_RUNTIME_DIR/modifier-owner.log"
target_log="$UMBRIEL_RUNTIME_DIR/modifier-target.log"
source_log="$UMBRIEL_RUNTIME_DIR/modifier-source.log"
"$UMBRIEL_UNMAP_CLIENT" modifier-keyboard-owner 400 400 > "$owner_log" 2>&1 &
"$UMBRIEL_DRAG_CLIENT" target-modifiers modifier-drop-target > "$target_log" 2>&1 &
target_pid=$!
"$UMBRIEL_DRAG_CLIENT" > "$source_log" 2>&1 &
source_pid=$!
await_lines "$target_log" 'ready$' 1
await_lines "$source_log" 'ready$' 1
"$UMBRIEL" settle > /dev/null
owner_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "modifier-keyboard-owner") | .id')
[[ -n $owner_id ]]
"$UMBRIEL" msg window-focus "$owner_id" > /dev/null

pointer_hold 1280 720 mod ctrl move 40 40 press 272 -- \
  move 1000 200 mark target hold \
  mod shift mark shift hold \
  mod none mark neutral hold \
  mod ctrl mark ctrl hold \
  move 0 600 mark outside hold \
  move 1000 200 mark reenter hold release 272 mod none
await_lines "$source_log" 'drag-started$' 1
pointer_step target
await_lines "$target_log" 'drag-enter$' 1
await_lines "$target_log" 'modifiers ctrl=true shift=false alt=false$' 1
pointer_step shift
await_lines "$target_log" 'modifiers ctrl=false shift=true alt=false$' 1
pointer_step neutral
await_lines "$target_log" 'modifiers ctrl=false shift=false alt=false$' 1
ctrl_count=$(events "$target_log" 'modifiers ctrl=true shift=false alt=false$')
pointer_step ctrl
await_lines "$target_log" 'modifiers ctrl=true shift=false alt=false$' "$((ctrl_count + 1))"
neutral_count=$(events "$target_log" 'modifiers ctrl=false shift=false alt=false$')
pointer_step outside
await_lines "$target_log" 'drag-leave$' 1
await_lines "$target_log" 'modifiers ctrl=false shift=false alt=false$' "$((neutral_count + 1))"
ctrl_count=$(events "$target_log" 'modifiers ctrl=true shift=false alt=false$')
pointer_step reenter
await_lines "$target_log" 'drag-enter$' 2
await_lines "$target_log" 'modifiers ctrl=true shift=false alt=false$' "$((ctrl_count + 1))"
if grep -q '^unexpected-keyboard-' "$target_log"; then
  echo "drag target received keyboard focus or keys: $(< "$target_log")"
  exit 1
fi
pointer_release
await_lines "$target_log" 'drop-received$' 1
await_lines "$source_log" 'drag-finished$' 1
wait "$target_pid"
wait "$source_pid"
echo "data-drag destinations received held/changed modifiers, leave reset and reentry without keys/focus"
