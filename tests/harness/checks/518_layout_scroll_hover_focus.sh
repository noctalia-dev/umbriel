#!/usr/bin/env bash
# Scrolling the strip can move an unfocused column beneath a stationary pointer. The next motion within that column
# must refresh hover focus without requiring a border crossing.
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
default_extent_fraction = 0.6

[animation]
duration_ms = 1

[input.focus]
follows_mouse = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

count=0
for title in scene-scroll-a scene-scroll-b scene-scroll-c; do
  spawn_client "$title"
  count=$((count + 1))
  wait_for_count "$count"
done
sleep 0.1

# Focus B at the left edge and consume any map-time hover invalidation. Thirteen 60-pixel scroll steps then carry B
# away and place C beneath the unchanged pointer position.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 100 360
wait_for_active scene-scroll-b
sleep 0.1
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 600 360
wait_for_active scene-scroll-b
for _ in $(seq 13); do
  "$UMBRIEL" msg layout-scroll-right > /dev/null
done
sleep 0.1
wait_for_active scene-scroll-b

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 601 360
wait_for_active scene-scroll-c

echo "motion refreshed hover focus after a command scrolled another column beneath the pointer"
