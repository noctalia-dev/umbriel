#!/usr/bin/env bash
# Focus that Umbriel picks after a window leaves lands on the tab a group shows, never on a hidden tab: closing a
# floated tab, a window above, below or beside the group, or the group's first tab, cycling focus, and sending a tab to
# the scratchpad.
set -euo pipefail

spawn_client() {
  "$UMBRIEL_UNMAP_CLIENT" "$1" 400 300 > /dev/null 2>&1 &
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq --arg title "$1" 'any(.[]; .title == $title)') == true ]] && return 0
    sleep 0.1
  done
  echo "expected window '$1': $("$UMBRIEL" windows --json)"
  return 1
}

# Three windows stacked into a group showing its last tab, `$1-3`.
spawn_group() {
  spawn_client "$1-1"
  spawn_client "$1-2"
  "$UMBRIEL" msg window-consume-left > /dev/null
  spawn_client "$1-3"
  "$UMBRIEL" msg window-consume-left > /dev/null
  "$UMBRIEL" msg column-toggle-tabbed > /dev/null
}

close_window() {
  local pid
  pid=$("$UMBRIEL" windows --json | jq -r "$1 | .pid")
  kill "$pid"
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq --argjson pid "$pid" 'any(.[]; .pid == $pid)') == false ]] && return 0
    sleep 0.1
  done
  echo "expected pid $pid to close: $("$UMBRIEL" windows --json)"
  return 1
}

close_focused() {
  close_window '.[] | select(.focused and .visible)'
}

expect_focus() {
  "$UMBRIEL" settle
  local windows
  windows=$("$UMBRIEL" windows --json)
  if ! jq -e --arg title "$2" '
    .[] | select(.title == $title) | .focused and .visible and (.tab_hidden | not)
  ' <<< "$windows" > /dev/null; then
    echo "$1: expected focus on the shown tab '$2': $windows"
    exit 1
  fi
}

spawn_group float
"$UMBRIEL" msg column-focus-tab 2 > /dev/null
"$UMBRIEL" msg window-toggle-floating > /dev/null
close_focused
expect_focus "closing a floated tab" float-1

"$UMBRIEL" msg workspace-switch 2 > /dev/null
spawn_group below
spawn_client below-window
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg window-move-down > /dev/null
"$UMBRIEL" msg window-focus-up > /dev/null
"$UMBRIEL" msg column-focus-tab 1 > /dev/null
"$UMBRIEL" msg window-focus-down > /dev/null
close_focused
expect_focus "closing the window below a group" below-1

"$UMBRIEL" msg workspace-switch 7 > /dev/null
spawn_group above
spawn_client above-window
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg window-move-up > /dev/null
"$UMBRIEL" msg window-focus-down > /dev/null
"$UMBRIEL" msg column-focus-tab 2 > /dev/null
"$UMBRIEL" msg window-focus-up > /dev/null
close_focused
expect_focus "closing the window above a group" above-2

"$UMBRIEL" msg workspace-switch 3 > /dev/null
spawn_group first
spawn_client first-window
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg window-move-up > /dev/null
"$UMBRIEL" msg window-focus-down > /dev/null
"$UMBRIEL" msg column-focus-tab 1 > /dev/null
close_focused
expect_focus "closing a group's first tab" first-2

"$UMBRIEL" msg workspace-switch 6 > /dev/null
spawn_group beside
"$UMBRIEL" msg column-focus-tab 2 > /dev/null
spawn_client beside-window
close_window '.[] | select(.title == "beside-2")'
close_focused
expect_focus "closing the window beside a group" beside-1

"$UMBRIEL" msg workspace-switch 4 > /dev/null
spawn_group cycle
"$UMBRIEL" msg column-focus-tab 2 > /dev/null
spawn_client cycle-window
"$UMBRIEL" msg window-focus-previous > /dev/null
expect_focus "focusing the previous window" cycle-2

"$UMBRIEL" msg workspace-switch 5 > /dev/null
spawn_group pad
"$UMBRIEL" msg column-focus-tab 2 > /dev/null
"$UMBRIEL" msg window-toggle-scratchpad > /dev/null
expect_focus "sending a tab to the scratchpad" pad-1

echo "focus landed on the shown tab every time"
