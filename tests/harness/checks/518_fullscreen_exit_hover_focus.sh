#!/usr/bin/env bash
# Leaving fullscreen can reveal an unfocused tiled window beneath a stationary pointer. Motion inside that tile must
# refresh hover focus without requiring a border crossing.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

spawn_client() { "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 & }

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.05
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_active() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end') == "$want" ]] && return 0
    sleep 0.05
  done
  echo "expected '$want' to be active: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "scrolling"

[layout.scrolling]
default_extent_fraction = 0.5

[animation]
duration_ms = 1

[input.focus]
follows_mouse = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client scene-fullscreen-left
wait_for_count 1
spawn_client scene-fullscreen-right
wait_for_count 2
sleep 0.1

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 100 360
wait_for_active scene-fullscreen-left
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
sleep 0.1

# The fullscreen window still owns this position, so the motion consumes no geometric enter into the hidden tile.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 1000 360
wait_for_active scene-fullscreen-left
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
sleep 0.1
wait_for_active scene-fullscreen-left

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 1001 360
wait_for_active scene-fullscreen-right

echo "motion refreshed hover focus after fullscreen exit revealed another tile beneath the pointer"
