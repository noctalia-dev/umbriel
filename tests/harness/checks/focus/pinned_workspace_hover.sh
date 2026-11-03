#!/usr/bin/env bash
# A pinned window follows its output to the newly active workspace. Leaving that window by hover must restore seat focus
# to the active workspace's window beneath the pointer.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

windows() { "$UMBRIEL" windows --json; }

spawn_client() {
  "$CLIENT" "$1" "${2:-1200}" "${3:-700}" > /dev/null 2>&1 &
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $(windows)"
  return 1
}

active_title() {
  windows | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end'
}

wait_for_active() {
  local want=$1
  for _ in $(seq 40); do
    [[ $(active_title) == "$want" ]] && return 0
    sleep 0.05
  done
  echo "expected '$want' to be active, got: $(windows)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[input.focus]
follows_mouse = true

[[window_rule]]
match.title = "^pinned-hover$"
default_floating = true
default_pinned = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client workspace-one
wait_for_count 1
spawn_client pinned-hover 300 200
wait_for_count 2
wait_for_active pinned-hover

"$UMBRIEL" msg workspace-switch 2 > /dev/null
spawn_client workspace-two
wait_for_count 3
wait_for_active workspace-two

# Focus the pinned window explicitly so the handoff does not depend on how the workspace switch itself selected focus.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 200 200
pin_id=$(windows | jq -r '.[] | select(.title == "pinned-hover") | .id')
"$UMBRIEL" msg window-focus "$pin_id" > /dev/null
wait_for_active pinned-hover

# Moving onto the workspace's own window must hand seat focus back to it.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 800 500
wait_for_active workspace-two

echo "leaving a pinned window restores hover focus to the active workspace"
