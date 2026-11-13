#!/usr/bin/env bash
# Sloppy hover refreshes after a new window changes the scene under a stationary pointer: when the window beneath it
# is one the pointer was not over before, the next motion focuses it even though both coordinates lie inside it. A new
# window that opens away from the pointer keeps focus while the pointer stays over the window it was already on.
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
default_extent_fraction = 0.5

[animation]
duration_ms = 1
curve = "linear"

[input.focus]
follows_mouse = "sloppy"

[[window_rule]]
match.title = "^spawn-hover-away$"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 900, y = 450, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

# This point is inside the first half-width tile once the windows map. Keep the pointer there while the second window
# maps and takes keyboard focus on the right.
pointer move 64 64
spawn_client "spawn-hover-left"
wait_for_count 1
spawn_client "spawn-hover-right"
wait_for_count 2
wait_for_active "spawn-hover-right"
sleep 0.1

# A one-pixel move stays inside the left tile. The map-time focus change must let it refresh hover focus once.
pointer move 65 64
wait_for_active "spawn-hover-left"

# The pointer was already over the left tile, so a window opening elsewhere keeps the focus it takes.
spawn_client "spawn-hover-away"
wait_for_count 3
wait_for_active "spawn-hover-away"
"$UMBRIEL" settle
pointer move 66 64
"$UMBRIEL" settle
wait_for_active "spawn-hover-away"

echo "motion focuses a window that appeared under the stationary pointer, but not over one opened elsewhere"
