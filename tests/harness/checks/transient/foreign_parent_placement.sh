#!/usr/bin/env bash
# A dialog parented through xdg-foreign, the way a portal attaches its file chooser to an application's window, opens
# centered over the window another process exported. It is attached to that window: it takes its focus and pointer,
# and the two move as one.
set -euo pipefail

readonly BTN_LEFT=272
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly PARENT_LOG="$UMBRIEL_RUNTIME_DIR/foreign-parent.log"
readonly UNRELATED_LOG="$UMBRIEL_RUNTIME_DIR/foreign-unrelated.log"
readonly CHILD_LOG="$UMBRIEL_RUNTIME_DIR/foreign-child.log"

windows() {
  "$UMBRIEL" windows --json
}

field_of() {
  local title=$1 field=$2
  windows | jq -r --arg title "$title" --arg field "$field" '.[] | select(.title == $title) | .[$field]'
}

wait_for_window_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $(windows)"
  return 1
}

wait_for_field() {
  local title=$1 field=$2 want=$3
  for _ in $(seq 60); do
    [[ $(field_of "$title" "$field") == "$want" ]] && return 0
    sleep 0.1
  done
  echo "expected $title $field=$want, got: $(windows)"
  return 1
}

wait_for_position() {
  local title=$1 x=$2 y=$3
  wait_for_field "$title" x "$x"
  wait_for_field "$title" y "$y"
}

pointer_enters() {
  grep -c '^pointer-enter' "$PARENT_LOG" || true
}

cat >> "$UMBRIEL_CONFIG" <<'EOF2'

[animation]
enabled = false

[input.focus]
follows_mouse = true

[[window_rule]]
match.title = "^foreign-parent$"
default_floating = true
default_position = { x = 0, y = 0, anchor = "top_left" }

[[window_rule]]
match.title = "^foreign-unrelated$"
default_floating = true
default_position = { x = 0, y = 0, anchor = "bottom_right" }
EOF2
"$UMBRIEL" msg config-reload > /dev/null

# The observers keep their own 640x480, so the parent covers 0,0 to 640,480 and the unrelated window 640,240 to
# 1280,720. The parent prints the handle it exported.
EXPORT_TOPLEVEL=1 "$OBSERVER" foreign-parent > "$PARENT_LOG" 2>&1 &
wait_for_window_count 1
handle=
for _ in $(seq 60); do
  handle=$(sed -n 's/^exported handle=//p' "$PARENT_LOG")
  [[ -n $handle ]] && break
  sleep 0.1
done
if [[ -z $handle ]]; then
  echo "the parent never received its xdg-foreign handle: $(cat "$PARENT_LOG")"
  exit 1
fi
"$OBSERVER" foreign-unrelated > "$UNRELATED_LOG" 2>&1 &
wait_for_window_count 2

"$POINTER" 1280 720 move 60 440
wait_for_field foreign-parent focused true
"$POINTER" 1280 720 move 1000 600
wait_for_field foreign-unrelated focused true
enters=$(pointer_enters)
if (( enters < 1 )); then
  echo "the parent never saw the pointer before the dialog opened: $(cat "$PARENT_LOG")"
  exit 1
fi

# Another process imports the handle for its 400x300 dialog, which lands centered over the parent, at 120,90, and
# takes the focus.
TRANSIENT_FOREIGN_HANDLE=$handle FOLLOW_CONFIGURES=1 "$CLIENT" foreign-child 400 300 > "$CHILD_LOG" 2>&1 &
wait_for_window_count 3
wait_for_position foreign-child 120 90
wait_for_field foreign-child focused true

"$POINTER" 1280 720 move 1000 600
wait_for_field foreign-unrelated focused true

# 60,440 is inside the parent and outside the dialog. Hovering there focuses the dialog, and the parent gets no pointer.
"$POINTER" 1280 720 move 60 440
wait_for_field foreign-child focused true
wait_for_field foreign-unrelated focused false
sleep 0.3 # real time: proves the parent gets no pointer enter
if [[ $(pointer_enters) -ne $enters ]]; then
  echo "the parent received the pointer while its dialog was open: $(cat "$PARENT_LOG")"
  exit 1
fi

# The drags below start and end on positions that are exact fractions of the 1280x720 output, so the virtual pointer
# lands on whole pixels and the deltas come out exact.

# Dragging the parent by 100,90 carries the dialog along.
"$POINTER" 1280 720 move 60 450 mod logo press "$BTN_LEFT" move 160 540 release "$BTN_LEFT" mod none
wait_for_position foreign-parent 100 90
wait_for_position foreign-child 220 180
wait_for_field foreign-child focused true

# Dragging the dialog by 50,90 drags the parent instead, and the dialog stays over it.
"$POINTER" 1280 720 move 300 270 mod logo press "$BTN_LEFT" move 350 360 release "$BTN_LEFT" mod none
wait_for_position foreign-parent 150 180
wait_for_position foreign-child 270 270

# The blocked parent cannot use a press, so a plain drag on it, by 50,45, moves it as Mod+drag would.
"$POINTER" 1280 720 move 200 630 press "$BTN_LEFT" move 250 675 release "$BTN_LEFT"
wait_for_position foreign-parent 200 225
wait_for_position foreign-child 320 315
wait_for_field foreign-child focused true

# Resizing the dialog keeps it centered on the parent, which now sits at 200,225.
"$UMBRIEL" msg window-set-primary-extent:0.5 > /dev/null
centered=
for _ in $(seq 60); do
  child=$(windows | jq -c '.[] | select(.title == "foreign-child") | [.w, .h, .x, .y]')
  read -r w h x y <<< "$(jq -r '@sh' <<< "$child" | tr -d "'")"
  if (( w != 400 && x == 200 + (640 - w) / 2 && y == 225 + (480 - h) / 2 )); then
    centered=1
    break
  fi
  sleep 0.1
done
if [[ -z $centered ]]; then
  echo "the resized dialog is not centered on its parent: $(windows)"
  exit 1
fi

# Closing the dialog hands the focus back to the parent.
"$UMBRIEL" msg window-close > /dev/null
wait_for_field foreign-parent focused true

echo "a dialog parented through xdg-foreign opens over the exported window and is attached to it"
