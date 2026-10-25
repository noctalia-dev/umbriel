#!/usr/bin/env bash
# harness: outputs=1
# A floating drag abandoned outside its drop path must still settle the window
# into the grabbable margin, exactly like a drop. Here the session locks
# mid-drag and the next motion resets the grab: the same resetMode ending the
# toplevel-drag detach and session-lock branches take.
# The drop settle is the only correction for where a drag parked a window, so
# losing it leaves the window stranded mostly out of frame with nothing to
# re-clamp it.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly LOCK_CLIENT="${UMBRIEL_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly TITLE=abandon-float
readonly LOCK_LOG="$UMBRIEL_RUNTIME_DIR/abandon-lock.log"
readonly LOCK_FIFO="$UMBRIEL_RUNTIME_DIR/abandon-lock-control"
readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "floating"

[animation]
enabled = false

[animation.windows_drag]
physics = false

[appearance]
border_width = 0
outer_border_width = 0

[[window_rule]]
match.title = "^abandon-float$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

ipc_box() {
  "$UMBRIEL" windows --json | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | "\(.x) \(.y) \(.w) \(.h)"'
}

"$CLIENT" "$TITLE" 400 300 > "$UMBRIEL_RUNTIME_DIR/abandon-float.log" 2>&1 &
for _ in $(seq 60); do
  [[ -n $(ipc_box) ]] && break
  sleep 0.25
done
"$UMBRIEL" settle

# Grab the title bar at the window's right edge and shove the window left until
# only a few pixels stay on screen: the margin (75px of the 400px window) wants
# x >= -325. The button stays held: a released drag runs the drop settle.
read -r wx wy ww wh < <(ipc_box)
gx=$((wx + ww - 5))
gy=$((wy + 20))
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  mod logo move "$gx" "$gy" press "$BTN_LEFT" move 2 "$gy" \
  -- move 30 300 mark abandoned hold release "$BTN_LEFT" mod none

# Lock mid-drag. The next motion while locked resets the grab without the drop
# settle: the drag is abandoned wherever it parked the window.
mkfifo "$LOCK_FIFO"
exec {lock_fd}<> "$LOCK_FIFO"
"$LOCK_CLIENT" <&"$lock_fd" > "$LOCK_LOG" 2>&1 &
for _ in $(seq 100); do
  grep -q '^locked$' "$LOCK_LOG" && break
  sleep 0.05
done
if ! grep -q '^locked$' "$LOCK_LOG"; then
  echo "the session never locked: $(cat "$LOCK_LOG")"
  exit 1
fi

pointer_step abandoned

echo unlock >&"$lock_fd"
for _ in $(seq 100); do
  grep -q '^unlocked$' "$LOCK_LOG" && break
  sleep 0.05
done
if ! grep -q '^unlocked$' "$LOCK_LOG"; then
  echo "the session never unlocked: $(cat "$LOCK_LOG")"
  exit 1
fi
pointer_release
"$UMBRIEL" settle

read -r wx wy ww wh < <(ipc_box)
visible_x=$((ww / 4))
((visible_x < 10)) && visible_x=10
((visible_x > 75)) && visible_x=75
visible_y=$((wh / 4))
((visible_y < 10)) && visible_y=10
((visible_y > 75)) && visible_y=75
min_x=$((visible_x - ww))
min_y=$((visible_y - wh))
if ((wx < min_x || wy < min_y)); then
  echo "the abandoned drag left the window outside the grabbable margin: $(ipc_box)"
  echo "margin for a ${ww}x${wh} window: x >= $min_x, y >= $min_y"
  exit 1
fi

echo "the abandoned drag settled into the grabbable margin"
