#!/usr/bin/env bash
# Returning to a workspace can replace the scene under a stationary pointer. The next motion must focus the window
# under that pointer even when the old and new coordinates remain inside the same tile.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 &
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

active_title() {
  "$UMBRIEL" windows --json | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end'
}

wait_for_active() {
  local want=$1
  for _ in $(seq 40); do
    [[ $(active_title) == "$want" ]] && return 0
    sleep 0.05
  done
  echo "expected '$want' to be active, got: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "scrolling"

[layout.scrolling]
default_width_fraction = 0.5

[animation]
duration_ms = 1

[input.focus]
follows_mouse = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client "workspace-hover-left"
wait_for_count 1
spawn_client "workspace-hover-right"
wait_for_count 2
wait_for_active "workspace-hover-right"
sleep 0.1

# Clear pointer focus on an empty workspace, then return without moving the pointer. Workspace focus history restores
# the right window while the pointer's position is over the left window.
"$UMBRIEL" msg workspace-switch:2 > /dev/null
pointer move 64 64
"$UMBRIEL" msg workspace-switch:1 > /dev/null
wait_for_active "workspace-hover-right"
sleep 0.1

# Both coordinates resolve to the left tile after the workspace returns. The workspace focus transition must let this
# motion refresh hover focus once and activate that tile.
pointer move 65 64
wait_for_active "workspace-hover-left"

echo "motion refreshes hover focus after returning to a workspace"
