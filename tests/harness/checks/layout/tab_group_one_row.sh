#!/usr/bin/env bash
# A tab group takes one row's share of its column, so a window stacked under it gets as much height as the window
# beside it, as in a column without a group.
set -euo pipefail

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

spawn_client tab-a
wait_for_windows 1
count=1
for title in tab-b tab-c tab-d; do
  spawn_client "$title"
  wait_for_windows "$((++count))"
  "$UMBRIEL" msg window-consume-left > /dev/null
done
"$UMBRIEL" settle
top=$("$UMBRIEL" windows --json | jq '[.[].y] | min')
"$UMBRIEL" msg column-toggle-tabbed > /dev/null
"$UMBRIEL" msg window-move-down > /dev/null
spawn_client below
wait_for_windows 5
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" settle

# The group's slot runs from the column's top to the row below it, bar included.
windows=$("$UMBRIEL" windows --json)
if ! jq -e --argjson top "$top" '
  (.[] | select(.title == "tab-d")) as $middle
  | (.[] | select(.title == "below")) as $below
  | ($below.y - $middle.y - $middle.h) as $gap
  | ($middle.y - $gap - $top) as $group
  | ($middle.h - $below.h | fabs) <= 1 and ($group - $middle.h | fabs) <= 1
' <<< "$windows" > /dev/null; then
  echo "expected the group, tab-d and 'below' to share the column evenly: $windows"
  exit 1
fi

echo "the group and the windows under it share the column evenly"
