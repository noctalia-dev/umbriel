#!/usr/bin/env bash
# Closing an exclusive layer (launcher, panel) gives focus back to whatever had
# it: the workspace window or the scratchpad. Background clicks still prefer
# the workspace (see 233).
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly SCRATCHPAD=refocus
readonly WS=scratchpad-refocus-workspace
readonly SCRATCH=scratchpad-refocus-foreground

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
scale = 0

[[scratchpad]]
name = "refocus"

[[window_rule]]
match.title = "^scratchpad-refocus-workspace$"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 50, y = 50, anchor = "top_left" }

[[window_rule]]
match.title = "^scratchpad-refocus-foreground$"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
default_position = { x = 430, y = 230, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }

field_of() {
  windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_count() {
  local expected=$1 count=
  for _ in $(seq 80); do
    count=$(windows | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected windows, got $count: $(windows)"
  return 1
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual': $(windows)"
  return 1
}

wait_for_ready() {
  for _ in $(seq 80); do
    grep -q '^ready$' "$1" && return 0
    sleep 0.1
  done
  echo "exclusive-zone panel did not map: $(< "$1")"
  return 1
}

steal_with_layer() {
  local log=$1 layer_pid=
  "$LAYER_CLIENT" HEADLESS-1 40 keyboard=exclusive > "$log" 2>&1 &
  layer_pid=$!
  wait_for_ready "$log" || return 1
  wait_for_field "$WS" active false || return 1
  wait_for_field "$SCRATCH" active false || return 1
  kill "$layer_pid" 2>/dev/null || true
  wait "$layer_pid" 2>/dev/null || true
}

"$CLIENT" "$WS" > "$UMBRIEL_RUNTIME_DIR/$WS.log" 2>&1 &
wait_for_count 1
wait_for_field "$WS" active true

"$CLIENT" "$SCRATCH" 420 260 > "$UMBRIEL_RUNTIME_DIR/$SCRATCH.log" 2>&1 &
wait_for_count 2
scratch_id=$(field_of "$SCRATCH" id)
ws_id=$(field_of "$WS" id)
"$UMBRIEL" msg "window-focus:$scratch_id" > /dev/null
"$UMBRIEL" msg "window-move-to-scratchpad:$SCRATCHPAD" > /dev/null
wait_for_field "$SCRATCH" scratchpad "$SCRATCHPAD"
"$UMBRIEL" msg "scratchpad-toggle:$SCRATCHPAD" > /dev/null
wait_for_field "$SCRATCH" active true
"$UMBRIEL" msg "window-focus:$scratch_id" > /dev/null
wait_for_field "$SCRATCH" active true

# Phase one: focus sits on the workspace, so the layer release must hand it
# back to the workspace, not the scratchpad.
"$UMBRIEL" msg "window-focus:$ws_id" > /dev/null
wait_for_field "$WS" active true
if ! wait_for_field "$SCRATCH" active false; then
  echo "workspace did not take focus from the visible scratchpad"
  exit 1
fi
steal_with_layer "$UMBRIEL_RUNTIME_DIR/layer-workspace.log"
wait_for_field "$WS" active true
if ! wait_for_field "$SCRATCH" active false; then
  echo "layer release stole workspace focus to the scratchpad"
  exit 1
fi

# Phase two: focus sits on the scratchpad, so the layer release must hand it
# back to the scratchpad, not the workspace behind it.
"$UMBRIEL" msg "window-focus:$scratch_id" > /dev/null
wait_for_field "$SCRATCH" active true
steal_with_layer "$UMBRIEL_RUNTIME_DIR/layer-scratchpad.log"
wait_for_field "$SCRATCH" active true
if ! wait_for_field "$WS" active false; then
  echo "layer release dropped scratchpad focus to the workspace"
  exit 1
fi

echo "layer release restores the previously focused workspace or scratchpad"
