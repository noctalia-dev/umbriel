#!/usr/bin/env bash
# Growing the window below a tab group, or dragging its edge, stops where the group still fits its bar and its tab's
# minimum height, as it stops at a window's minimum: the bar stays in place, the tab never overlaps the window below,
# and the next drag moves the group at once.
set -euo pipefail

readonly BTN_RIGHT=273

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') == "$1" ]] && return 0
    sleep 0.1
  done
  echo "expected $1 window(s): $("$UMBRIEL" windows --json)"
  return 1
}

bottom_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .y + .h'
}

spawn_client tab-a
wait_for_windows 1
spawn_client tab-b
wait_for_windows 2
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg column-toggle-tabbed > /dev/null
spawn_client below
wait_for_windows 3
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg window-move-down > /dev/null
"$UMBRIEL" settle
tab_y=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "tab-b") | .y')

for _ in $(seq 10); do
  "$UMBRIEL" msg window-modify-secondary-extent 0.1 > /dev/null
done
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
if ! jq -e --argjson tab_y "$tab_y" '
  (.[] | select(.title == "tab-b")) as $tab
  | (.[] | select(.title == "below")) as $below
  | $tab.y == $tab_y and $tab.y + $tab.h < $below.y
' <<< "$windows" > /dev/null; then
  echo "expected tab-b to stay at y $tab_y above 'below': $windows"
  exit 1
fi

# Mod+right-drags tab-b's bottom edge, which is the group's, to `$1`.
drag_edge_to() {
  local edge x y
  edge='.[] | select(.title == "tab-b") | "\(.x + 100) \(.y + .h - 5)"'
  read -r x y <<< "$("$UMBRIEL" windows --json | jq -r "$edge")"
  "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo move "$x" "$y" press "$BTN_RIGHT" move "$x" "$1" release "$BTN_RIGHT" \
    mod none
  "$UMBRIEL" settle
}

# A group held at its minimum follows the next drag at once, so it never stores less than it shows.
expect_drag_follows() {
  local bottom
  bottom=$(bottom_of tab-b)
  drag_edge_to "$((bottom + 35))"
  if [[ $(bottom_of tab-b) != "$((bottom + 40))" ]]; then
    echo "$1: expected tab-b's bottom to follow the drag from $bottom to $((bottom + 40)): $("$UMBRIEL" windows --json)"
    exit 1
  fi
}

expect_drag_follows "after growing 'below'"
drag_edge_to 15
expect_drag_follows "after dragging past the minimum"

echo "the group kept room for its bar and its tab"
