#!/usr/bin/env bash
# Hovering an exposed scrolling column reveals it. While that reveal moves the strip beneath a nearly stationary
# pointer, each newly passing column must not be mistaken for another pointer enter and start a focus cascade.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

spawn_client() {
  "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 &
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.05
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

active_title() {
  "$UMBRIEL" windows --json | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end'
}

wait_for_active() {
  local want=$1
  for _ in $(seq 100); do
    [[ $(active_title) == "$want" ]] && return 0
    sleep 0.01
  done
  echo "expected '$want' to be active, got: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "scrolling"

[layout.scrolling]
default_width_fraction = 0.6
center_focused = true

[animation]
duration_ms = 1200

[input.focus]
follows_mouse = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

count=0
for title in hover-stability-a hover-stability-b hover-stability-c; do
  spawn_client "$title"
  count=$((count + 1))
  wait_for_count "$count"
done
wait_for_active hover-stability-c

# The last column rests centered at x=264. The previous column extends from x=-499 to x=252, leaving the trigger
# point inside it. Wait for map-time position animations so this starting presentation is deterministic.
sleep 1.4

# Keep one virtual pointer alive for the whole sequence. Establish pointer focus on C, cross once into B, then move
# only one pixel vertically while B's reveal carries A horizontally underneath x=20.
pointer_args=(move 640 360 pause 80 move 20 360 pause 40)
for i in $(seq 1 70); do
  pointer_args+=(move 20 "$((360 + i % 2))" pause 20)
done
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" "${pointer_args[@]}" &
pointer_pid=$!

wait_for_active hover-stability-b
while kill -0 "$pointer_pid" 2>/dev/null; do
  current=$(active_title)
  if [[ $current != hover-stability-b ]]; then
    kill "$pointer_pid" 2>/dev/null || true
    wait "$pointer_pid" 2>/dev/null || true
    echo "hover focus cascaded from B to '$current' while the strip animated: $("$UMBRIEL" windows --json)"
    exit 1
  fi
  sleep 0.01
done
wait "$pointer_pid"

if [[ $(active_title) != hover-stability-b ]]; then
  echo "hover focus did not remain on B after its reveal: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "one hover reveal stayed focused through 70 one-pixel motions"
