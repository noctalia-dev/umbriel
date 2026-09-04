#!/usr/bin/env bash
# Pointer hit-testing: a click focuses the window under the cursor. This is the check that was missing when a change to View's base classes moved its SceneNode subobject off offset zero, so Server::viewAt returned null for every window and click-to-focus, interactive move, and interactive resize all stopped working. Nothing else in the harness touches that path: every other check drives the compositor through IPC actions, which never hit-test. The headless backend has no input devices, so the cursor is driven through zwlr_virtual_pointer_v1 by tests/harness/clients/pointer_client.cpp. The compositor attaches it to its wlr_cursor like a physical mouse, so these events take the same path real input does.
set -euo pipefail

readonly BTN_LEFT=272 # evdev BTN_LEFT
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

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

# x of the focused window on the active workspace, or "none".
focused_x() {
  "$UMBRIEL" windows --json | jq -r \
    '[.[] | select(.focused and .active) | .x] | if length == 1 then .[0] else "none" end'
}

wait_for_focus_at() {
  local want=$1
  for _ in $(seq 40); do
    [[ $(focused_x) == "$want" ]] && return 0
    sleep 0.25
  done
  echo "expected the window at x=$want to be focused, focus is at $(focused_x)"
  echo "  windows: $("$UMBRIEL" windows --json | jq -c '[.[] | {x, focused, active}]')"
  return 1
}

wait_for_window_x() {
  local id=$1 want=$2 actual=none
  for _ in $(seq 40); do
    actual=$("$UMBRIEL" windows --json | jq -r --arg id "$id" '.[] | select(.id == $id) | .x')
    [[ $actual == "$want" ]] && return 0
    sleep 0.25
  done
  echo "expected window $id at x=$want, got x=$actual"
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

# Directional focus leaves the cursor in place while follows_focus has its
# default disabled value. Focus moves right, then an unmoved click returns it
# to the left window under the cursor.
pointer move "$LEFT_X" "$MID_Y" click "$BTN_LEFT"
wait_for_focus_at 10
"$UMBRIEL" msg window-focus-right > /dev/null
wait_for_focus_at 646
pointer click "$BTN_LEFT"
wait_for_focus_at 10

# With follows_focus enabled, the same navigation moves the cursor into the
# newly focused right window. Plain window-focus remains focus-only, so moving
# keyboard focus left and clicking without pointer motion must return focus to
# the right window under the warped cursor.
printf '\n[input.cursor]\nfollows_focus = true\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg window-focus-right > /dev/null
wait_for_focus_at 646
"$UMBRIEL" msg "window-focus:$left_id" > /dev/null
wait_for_focus_at 10
pointer click "$BTN_LEFT"
wait_for_focus_at 646

# Workspace focus history is local to the focused output. Returning to that
# output's previous workspace is still focus navigation, so follows_focus must
# move the cursor to its restored focused window. Leave a second window under
# the old pointer position, then click without motion after the transition.
"$UMBRIEL" msg window-focus-left > /dev/null
wait_for_focus_at 10
"$UMBRIEL" msg workspace-switch:2 > /dev/null
spawn_client
wait_for_count 3
pointer move "$RIGHT_X" "$MID_Y"
"$UMBRIEL" msg workspace-focus-last > /dev/null
wait_for_focus_at 10
sleep 1
pointer click "$BTN_LEFT"
wait_for_focus_at 10

# Moving the focused left window to workspace 2 places it to the right of the
# window already there. Match the reported follows-mouse setup. The cursor must
# follow the moved window, so a focus-only detour followed by an unmoved click
# returns focus to it.
printf '\n[input.focus]\nfollows_mouse = true\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
destination_id=$("$UMBRIEL" windows --json | jq -r \
  --arg left "$left_id" --arg right "$right_id" '.[] | select(.id != $left and .id != $right) | .id')
"$UMBRIEL" msg "window-move-to-workspace:2" > /dev/null
wait_for_window_x "$left_id" 646
sleep 1
wait_for_focus_at 646
"$UMBRIEL" msg "window-focus:$destination_id" > /dev/null
wait_for_focus_at 10
pointer click "$BTN_LEFT"
wait_for_focus_at 646

echo "click focus and configured focus-navigation and workspace-transfer cursor warps are correct"
