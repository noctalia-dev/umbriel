#!/usr/bin/env bash
# Closing a focused row in a scrolling column can resize another row beneath a stationary pointer. With
# follows_mouse enabled, that revealed row must receive focus without requiring another pointer event.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

pointer_pid=
cleanup() {
  if [[ -n $pointer_pid ]]; then
    kill "$pointer_pid" 2>/dev/null || true
    wait "$pointer_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

spawn_client() {
  local title=$1
  "$CLIENT" "$title" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
}

windows() { "$UMBRIEL" windows --json; }

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $(windows)"
  return 1
}

field_of() {
  windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_active() {
  local title=$1
  for _ in $(seq 40); do
    [[ $(field_of "$title" active) == true ]] && return 0
    sleep 0.05
  done
  echo "expected '$title' to be active: $(windows)"
  return 1
}

wait_for_row_at_pointer() {
  local title=$1 y=$2
  for _ in $(seq 40); do
    if windows | jq -e --arg title "$title" --argjson y "$y" '
      .[]
      | select(.title == $title)
      | .y <= $y
    ' > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "expected '$title' to expand across pointer row $y: $(windows)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "scrolling"

[animation]
duration_ms = 1
curve = "linear"

[input.focus]
follows_mouse = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client scroll-hover-top
wait_for_count 1
spawn_client scroll-hover-middle
wait_for_count 2
"$UMBRIEL" msg window-consume-left > /dev/null
spawn_client scroll-hover-bottom
wait_for_count 3
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" settle

middle_id=$(field_of scroll-hover-middle id)
middle_x=$(field_of scroll-hover-middle x)
bottom_y=$(field_of scroll-hover-bottom y)
pointer_x=$((middle_x + 50))
pointer_y=$((bottom_y - 30))

# Keep the virtual pointer alive and stationary through the close. The lower part of the middle row is inherited by
# the bottom row after the remaining rows expand, while normal close replacement selects the top row.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$pointer_x" "$pointer_y" pause 10000 \
  > "$UMBRIEL_RUNTIME_DIR/scrolling-close-pointer.log" 2>&1 &
pointer_pid=$!
wait_for_active scroll-hover-middle

"$UMBRIEL" msg "window-close:$middle_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/scroll-hover-middle.log" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/scroll-hover-middle.log"; then
  echo "middle row did not unmap: $(< "$UMBRIEL_RUNTIME_DIR/scroll-hover-middle.log")"
  exit 1
fi
wait_for_count 2
"$UMBRIEL" settle
wait_for_row_at_pointer scroll-hover-bottom "$pointer_y"
wait_for_active scroll-hover-bottom

echo "scrolling close focused the row revealed beneath a stationary pointer"
