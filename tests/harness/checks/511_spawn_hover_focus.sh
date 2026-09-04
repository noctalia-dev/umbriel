#!/usr/bin/env bash
# When a new window changes focus and the scene under a stationary pointer, the next motion gets one hover refresh.
# Both coordinates are already inside the newly exposed window, so geometric enter detection alone cannot see it.
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

echo "motion focuses a window that appeared under the stationary pointer"
