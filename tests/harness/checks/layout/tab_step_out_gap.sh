#!/usr/bin/env bash
# A tab stepping out below a shrunk group that is alone in its column lands in the free space under it, as a window
# joining the column does: it reaches the column's bottom and the group keeps its height.
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

bottom_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .y + .h'
}

spawn_client tab-a
wait_for_windows 1
spawn_client tab-b
wait_for_windows 2
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg column-toggle-tabbed > /dev/null
"$UMBRIEL" settle
column_bottom=$(bottom_of tab-b)

for _ in 1 2 3; do
  "$UMBRIEL" msg window-modify-secondary-extent:-0.1 > /dev/null
done
"$UMBRIEL" settle
group_bottom=$(bottom_of tab-b)

"$UMBRIEL" msg window-move-down > /dev/null
"$UMBRIEL" settle

if [[ $(bottom_of tab-b) != "$column_bottom" || $(bottom_of tab-a) != "$group_bottom" ]]; then
  echo "expected tab-b to reach $column_bottom under the group ending at $group_bottom: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "the stepped-out tab filled the free space below its group"
