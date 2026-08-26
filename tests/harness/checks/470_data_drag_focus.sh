#!/usr/bin/env bash
# A data-device drag owns wlroots' keyboard grab. If Umbriel changes logical focus while that grab is active, ending
# the drag must replay the selected view into the default seat grab before the next key is delivered.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly LEFT_BUTTON=272
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
readonly DRAG_CLIENT="${UMBRIEL_DRAG_CLIENT:-./build-debug/drag-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly DRAG_LOG="$UMBRIEL_RUNTIME_DIR/data-drag.log"
readonly POINTER_LOG="$UMBRIEL_RUNTIME_DIR/data-drag-pointer.log"
readonly SOURCE_LOG="$UMBRIEL_RUNTIME_DIR/data-drag-source.log"
readonly TARGET_LOG="$UMBRIEL_RUNTIME_DIR/data-drag-target.log"

spawn_client() {
  "$CLIENT" "$1" 624 700 > "$2" 2>&1 &
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

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client "data-drag-source" "$SOURCE_LOG"
wait_for_count 1
spawn_client "data-drag-target" "$TARGET_LOG"
wait_for_count 2
sleep 0.1

windows=$("$UMBRIEL" windows --json)
source_id=$(jq -r '.[] | select(.title == "data-drag-source") | .id' <<< "$windows")
target_id=$(jq -r '.[] | select(.title == "data-drag-target") | .id' <<< "$windows")
target_x=$(jq -r '.[] | select(.title == "data-drag-target") | (.x + .w / 2 | round)' <<< "$windows")
target_y=$(jq -r '.[] | select(.title == "data-drag-target") | (.y + .h / 2 | round)' <<< "$windows")

"$DRAG_CLIENT" > "$DRAG_LOG" 2>&1 &
DRAG_PID=$!
for _ in $(seq 50); do
  grep -q '^ready$' "$DRAG_LOG" 2>/dev/null && break
  sleep 0.1
done
if ! grep -q '^ready$' "$DRAG_LOG" 2>/dev/null; then
  echo "data-device drag source did not map: $(< "$DRAG_LOG")"
  exit 1
fi

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  pause 1000 move 32 32 press "$LEFT_BUTTON" move "$target_x" "$target_y" pause 1000 release "$LEFT_BUTTON" tap 30 \
  > "$POINTER_LOG" 2>&1 &
POINTER_PID=$!
sleep 0.1

# The virtual keyboard must exist before source focus is established so its client receives the initial keyboard enter.
"$UMBRIEL" msg "window-focus:$source_id" > /dev/null
if [[ $(active_title) != data-drag-source ]]; then
  echo "could not establish source keyboard focus: $("$UMBRIEL" windows --json)"
  exit 1
fi

for _ in $(seq 50); do
  grep -q '^drag-started$' "$DRAG_LOG" 2>/dev/null && break
  sleep 0.1
done
if ! grep -q '^drag-started$' "$DRAG_LOG" 2>/dev/null; then
  echo "data-device drag did not start: $(< "$DRAG_LOG")"
  exit 1
fi

# This is the same logical focus change produced by crossing onto another output with focus-follows-mouse. The drag's
# keyboard grab discards the seat enter, but the target's active state and border still change immediately.
"$UMBRIEL" msg "window-focus:$target_id" > /dev/null
if [[ $(active_title) != data-drag-target ]]; then
  echo "target did not become visibly focused during the drag: $("$UMBRIEL" windows --json)"
  exit 1
fi

wait "$POINTER_PID" || {
  echo "pointer client failed: $(< "$POINTER_LOG")"
  exit 1
}
wait "$DRAG_PID" || {
  echo "drag client failed: $(< "$DRAG_LOG")"
  exit 1
}

for _ in $(seq 40); do
  grep -q '^key 30 1$' "$TARGET_LOG" 2>/dev/null && break
  sleep 0.05
done
if ! grep -q '^key 30 1$' "$TARGET_LOG" 2>/dev/null; then
  echo "typing returned to the drag source after the target became active: source=$(< "$SOURCE_LOG") target=$(< "$TARGET_LOG")"
  exit 1
fi
if grep -q '^key 30 1$' "$SOURCE_LOG" 2>/dev/null; then
  echo "drag source received typing after the target became active: source=$(< "$SOURCE_LOG") target=$(< "$TARGET_LOG")"
  exit 1
fi

echo "data-device drag completion restored keyboard focus to the active target"
