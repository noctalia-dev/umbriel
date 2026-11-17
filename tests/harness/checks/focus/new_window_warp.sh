#!/usr/bin/env bash
# harness: outputs=2
# follows_focus warps the pointer to a newly mapped window that receives initial keyboard focus, including a window
# that opens on another output. With the setting off, a new window still takes focus but the pointer stays where it was.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BTN_LEFT=272
readonly OUTPUT_W=2560
readonly OUTPUT_H=720

windows() { "$UMBRIEL" windows --json; }

wait_for_windows() {
  local want=$1
  for _ in $(seq 80); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got $(windows)"
  return 1
}

wait_for_active() {
  local id=$1
  for _ in $(seq 80); do
    [[ $(active_id) == "$id" ]] && return 0
    sleep 0.1
  done
  echo "expected $id to be active: $(windows)"
  return 1
}

window_id() { windows | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'; }
window_center_x() { windows | jq -r --arg id "$1" '.[] | select(.id == $id) | (.x + .w / 2 | floor)'; }
window_center_y() { windows | jq -r --arg id "$1" '.[] | select(.id == $id) | (.y + .h / 2 | floor)'; }
active_id() { windows | jq -r '.[] | select(.active) | .id'; }

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[layout.scrolling]
default_extent_fraction = 0.5

[input.cursor]
follows_focus = true

[[window_rule]]
match.title = "^follow-open-old$"
default_output = "HEADLESS-1"

[[window_rule]]
match.title = "^follow-open-new$"
default_output = "HEADLESS-2"

[[window_rule]]
match.title = "^follow-open-off$"
default_output = "HEADLESS-2"
EOF
"$UMBRIEL" msg config-reload > /dev/null

APP_ID=follow-open-old "$CLIENT" follow-open-old 420 260 > "$UMBRIEL_RUNTIME_DIR/follow-open-old.log" 2>&1 &
wait_for_windows 1
old_id=$(window_id follow-open-old)
"$UMBRIEL" msg window-focus "$old_id" > /dev/null
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$(window_center_x "$old_id")" "$(window_center_y "$old_id")"

# The new window opens on the other output and takes initial focus. follows_focus carries the pointer across, so a
# click at the pointer's current position keeps the new window focused. Without the warp the click lands on the older
# window and focuses that instead.
APP_ID=follow-open-new "$CLIENT" follow-open-new 420 260 > "$UMBRIEL_RUNTIME_DIR/follow-open-new.log" 2>&1 &
wait_for_windows 2
new_id=$(window_id follow-open-new)
"$UMBRIEL" settle
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" click "$BTN_LEFT"
wait_for_active "$new_id"

# With follows_focus off, the new window still takes focus but leaves the pointer on the previous window, so the same
# click returns focus there.
sed -i 's/^follows_focus = true$/follows_focus = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$(window_center_x "$old_id")" "$(window_center_y "$old_id")"
APP_ID=follow-open-off "$CLIENT" follow-open-off 420 260 > "$UMBRIEL_RUNTIME_DIR/follow-open-off.log" 2>&1 &
wait_for_windows 3
off_id=$(window_id follow-open-off)
"$UMBRIEL" settle
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" click "$BTN_LEFT"
for _ in $(seq 80); do
  [[ $(active_id) != "$off_id" ]] && break
  sleep 0.1
done
[[ $(active_id) != "$off_id" ]] || {
  echo "follows_focus = false still warped the pointer to the newly mapped window: $(windows)"
  exit 1
}

echo "follows_focus warps to a window that receives initial focus across outputs, and stays put when disabled"
