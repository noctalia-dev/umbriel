#!/usr/bin/env bash
# Pointer hit-testing: a click focuses the window under the cursor. This is the check that was missing when a change to View's base classes moved its SceneNode subobject off offset zero, so Server::viewAt returned null for every window and click-to-focus, interactive move, and interactive resize all stopped working. Nothing else in the harness touches that path: every other check drives the compositor through IPC actions, which never hit-test. The headless backend has no input devices, so the cursor is driven through zwlr_virtual_pointer_v1 by tests/harness/clients/pointer_client.cpp. The compositor attaches it to its wlr_cursor like a physical mouse, so these events take the same path real input does.
set -euo pipefail

readonly BTN_LEFT=272 # evdev BTN_LEFT
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"

printf '\n[layout.scrolling]\ndefault_width_fraction = 0.5\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

if [[ ! -x $POINTER ]]; then
  echo "pointer client not built at $POINTER"
  exit 1
fi

spawn_client() {
  foot sh -c 'sleep 120' > /dev/null 2>&1 &
}

# The output size is the pointer client's coordinate space, not an environment
# concern, so this wrapper stays.
pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

wait_for_count() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s)"
  return 1
}

# x of the focused window, or "none".
focused_x() { "$UMBRIEL" windows --json | jq -r '[.[] | select(.focused) | .x] | if length == 1 then .[0] else "none" end'; }

wait_for_focus_at() {
  local want=$1
  for _ in $(seq 40); do
    [[ $(focused_x) == "$want" ]] && return 0
    sleep 0.25
  done
  echo "expected the window at x=$want to be focused, focus is at $(focused_x)"
  echo "  windows: $("$UMBRIEL" windows --json | jq -c '[.[] | {x, focused}]')"
  return 1
}

spawn_client
wait_for_count 1
spawn_client
wait_for_count 2

# Two 624-wide columns at x=10 and x=646, both 700 tall from y=10.
readonly LEFT_X=322   # 10 + 624/2
readonly RIGHT_X=958  # 646 + 624/2
readonly MID_Y=360

# Whichever window has focus after mapping, clicking the left column must move
# focus there.
pointer move "$LEFT_X" "$MID_Y" click "$BTN_LEFT"
wait_for_focus_at 10

# Motion alone must not move focus: follows_mouse is off by default. This also
# proves the next assertion is the click's doing and not the hover's.
pointer move "$RIGHT_X" "$MID_Y"
sleep 0.5
if [[ $(focused_x) != "10" ]]; then
  echo "hovering moved focus with follows_mouse off: focus is at $(focused_x)"
  exit 1
fi

pointer click "$BTN_LEFT"
wait_for_focus_at 646

# And back, so a single stuck focus cannot pass by accident.
pointer move "$LEFT_X" "$MID_Y" click "$BTN_LEFT"
wait_for_focus_at 10

# A switcher-style focus moves the cursor with the selected window. Ordinary
# focus remains focus-only, so this sequence leaves keyboard focus on the left
# while the cursor stays over the right. The click must return focus to right.
right_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.x == 646) | .id')
left_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.x == 10) | .id')
"$UMBRIEL" msg "window-focus-warp:$right_id" > /dev/null
wait_for_focus_at 646
"$UMBRIEL" msg "window-focus:$left_id" > /dev/null
wait_for_focus_at 10
pointer click "$BTN_LEFT"
wait_for_focus_at 646

echo "click focus, hover behavior, and switcher cursor warp are correct"
