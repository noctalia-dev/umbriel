#!/usr/bin/env bash
# Switching workspaces must not steal keyboard focus from a visible scratchpad.
# Regression for #245: with the scratchpad open and focused, workspace-switch
# used to fall back to the new workspace's window while the scratchpad stayed
# in the foreground, so input went to a non-foreground window. refocus(Output*)
# retains the scratchpad in that case; explicit actions use refocusExplicit().
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SCRATCHPAD=switch
readonly WS1=scratchpad-switch-ws1
readonly WS2=scratchpad-switch-ws2
readonly SCRATCH=scratchpad-switch-foreground

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
scale = 0

[[scratchpad]]
name = "switch"

[[window_rule]]
match.title = "^scratchpad-switch-foreground$"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
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

"$CLIENT" "$WS1" > "$UMBRIEL_RUNTIME_DIR/$WS1.log" 2>&1 &
wait_for_count 1
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$CLIENT" "$WS2" > "$UMBRIEL_RUNTIME_DIR/$WS2.log" 2>&1 &
wait_for_count 2
"$UMBRIEL" msg workspace-switch:1 > /dev/null
wait_for_field "$WS1" active true

"$CLIENT" "$SCRATCH" 420 260 > "$UMBRIEL_RUNTIME_DIR/$SCRATCH.log" 2>&1 &
wait_for_count 3
scratch_id=$(field_of "$SCRATCH" id)
"$UMBRIEL" msg "window-focus:$scratch_id" > /dev/null
"$UMBRIEL" msg "window-move-to-scratchpad:$SCRATCHPAD" > /dev/null
wait_for_field "$SCRATCH" scratchpad "$SCRATCHPAD"
"$UMBRIEL" msg "scratchpad-toggle:$SCRATCHPAD" > /dev/null
wait_for_field "$SCRATCH" active true
"$UMBRIEL" msg "window-focus:$scratch_id" > /dev/null
wait_for_field "$SCRATCH" active true

# The workspace switch under test: focus must stay on the foreground scratchpad.
"$UMBRIEL" msg workspace-switch:2 > /dev/null
wait_for_field "$SCRATCH" active true
for _ in $(seq 5); do
  sleep 0.1
  if [[ $(field_of "$SCRATCH" active) != true || $(field_of "$WS2" active) != false ]]; then
    echo "workspace switch stole focus from the visible scratchpad: $(windows)"
    exit 1
  fi
done

echo "workspace switch keeps focus on the visible scratchpad"
