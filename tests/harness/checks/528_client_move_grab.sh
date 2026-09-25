#!/usr/bin/env bash
# harness: outputs=1
# An xdg_toplevel.move request must use the serial from a real pointer press.
# Once accepted, the requester loses pointer focus and the compositor owns the
# physical button until release. That release must finish the move and restore
# ordinary input delivery to a different surface.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly SOURCE=client-move-source
readonly TARGET=client-move-target
readonly MOVE_DX=260
readonly MOVE_DY=140
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly SOURCE_LOG="$UMBRIEL_RUNTIME_DIR/client-move-source.log"
readonly TARGET_LOG="$UMBRIEL_RUNTIME_DIR/client-move-target.log"
readonly POINTER_LOG="$UMBRIEL_RUNTIME_DIR/client-move-pointer.log"

if [[ ! -x $POINTER || ! -x $OBSERVER ]]; then
  echo "input helpers are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[[window_rule]]
match.title = "^client-move-source$"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 120, y = 100, anchor = "top_left" }

[[window_rule]]
match.title = "^client-move-target$"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 840, y = 410, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

await_windows() {
  local expected=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $expected ]] && return 0
    sleep 0.1
  done
  echo "expected $expected windows, got $("$UMBRIEL" windows --json | jq -c 'map(.title)')"
  return 1
}

window_box() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" \
    '.[] | select(.title == $title) | "\(.x) \(.y) \(.w) \(.h)"'
}

wait_for_position() {
  local title=$1 expected_x=$2 expected_y=$3 actual=
  for _ in $(seq 40); do
    actual=$("$UMBRIEL" windows --json | jq -r --arg title "$title" \
      '.[] | select(.title == $title) | "\(.x) \(.y)"')
    [[ $actual == "$expected_x $expected_y" ]] && return 0
    sleep 0.1
  done
  echo "expected $title at $expected_x,$expected_y, got ${actual:-no window}"
  return 1
}

"$OBSERVER" "$SOURCE" move-on-press > "$SOURCE_LOG" 2>&1 &
await_windows 1
"$OBSERVER" "$TARGET" > "$TARGET_LOG" 2>&1 &
await_windows 2

read -r source_x source_y source_w source_h < <(window_box "$SOURCE")
read -r target_x target_y target_w target_h < <(window_box "$TARGET")
source_center_x=$((source_x + source_w / 2))
source_center_y=$((source_y + source_h / 2))
drag_x=$((source_center_x + MOVE_DX))
drag_y=$((source_center_y + MOVE_DY))
target_center_x=$((target_x + target_w / 2))
target_center_y=$((target_y + target_h / 2))

# Both interactions use one virtual device. Pausing after the first press lets
# the source dispatch that event and send its xdg_toplevel.move request before
# the drag motion arrives.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  move "$source_center_x" "$source_center_y" pause 300 \
  press "$BTN_LEFT" pause 300 \
  move "$drag_x" "$drag_y" pause 100 release "$BTN_LEFT" pause 100 \
  move "$target_center_x" "$target_center_y" pause 100 click "$BTN_LEFT" \
  > "$POINTER_LOG" 2>&1 || {
  echo "pointer client failed: $(< "$POINTER_LOG")"
  exit 1
}

expected_x=$((source_x + MOVE_DX))
expected_y=$((source_y + MOVE_DY))
wait_for_position "$SOURCE" "$expected_x" "$expected_y"

source_sequence=$(grep -E 'pointer-(enter|leave|button)|move-requested' "$SOURCE_LOG" | tr '\n' '|')
if [[ $source_sequence != *"pointer-button code=$BTN_LEFT state=pressed|move-requested|pointer-leave"* ]]; then
  echo "the move request was not accepted with an immediate pointer leave: $source_sequence"
  exit 1
fi

target_sequence=$(grep -E 'pointer-(enter|leave|button)' "$TARGET_LOG" | tr '\n' '|')
if [[ $target_sequence != *"pointer-enter|pointer-button code=$BTN_LEFT state=pressed|pointer-button code=$BTN_LEFT state=released"* ]]; then
  echo "the target did not receive a normal click after the move release: $target_sequence"
  exit 1
fi
target_presses=$(grep -c "pointer-button code=$BTN_LEFT state=pressed" "$TARGET_LOG" || true)
target_releases=$(grep -c "pointer-button code=$BTN_LEFT state=released" "$TARGET_LOG" || true)
if ((target_presses != 1 || target_releases != 1)); then
  echo "expected one target press and release, got $target_presses and $target_releases"
  exit 1
fi

# A window that moves under a stationary cursor must see the pointer where it
# is drawn before the next press, not where it was before the move.
source_id=$("$UMBRIEL" windows --json | jq -r --arg title "$SOURCE" '.[] | select(.title == $title) | .id')
"$UMBRIEL" msg "window-focus:$source_id" > /dev/null
hover_x=$((expected_x + source_w - 20))
hover_y=$((expected_y + source_h - 20))
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$hover_x" "$hover_y" > "$POINTER_LOG" 2>&1 || {
  echo "pointer client failed: $(< "$POINTER_LOG")"
  exit 1
}
"$UMBRIEL" msg window-center > /dev/null
for _ in $(seq 40); do
  read -r centered_x centered_y _ _ < <(window_box "$SOURCE")
  [[ "$centered_x $centered_y" != "$expected_x $expected_y" ]] && break
  sleep 0.1
done
if ((hover_x < centered_x || hover_x >= centered_x + source_w || hover_y < centered_y || hover_y >= centered_y + source_h)); then
  echo "centering moved $SOURCE to $centered_x,$centered_y, off the cursor at $hover_x,$hover_y"
  exit 1
fi
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" pause 200 click "$BTN_LEFT" > "$POINTER_LOG" 2>&1 || {
  echo "pointer client failed: $(< "$POINTER_LOG")"
  exit 1
}
position=
for _ in $(seq 40); do
  position=$(grep 'press-position' "$SOURCE_LOG" | tail -n 1)
  [[ $(grep -c 'press-position' "$SOURCE_LOG") -ge 2 ]] && break
  sleep 0.1
done
expected_position="press-position x=$((hover_x - centered_x)) y=$((hover_y - centered_y))"
if [[ $position != "$expected_position" ]]; then
  echo "press after the window moved under the cursor: got '$position', expected '$expected_position'"
  exit 1
fi

echo "client-requested move changed geometry and released input to a different window"
