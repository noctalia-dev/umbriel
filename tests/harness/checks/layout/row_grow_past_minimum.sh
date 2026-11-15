#!/usr/bin/env bash
# A row at its minimum height does not stop another row of its column from growing: the growth comes from the rows
# that have room, and the row at its minimum keeps its height.
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

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

spawn_client top
wait_for_windows 1
spawn_client middle
wait_for_windows 2
"$UMBRIEL" msg window-consume-left > /dev/null
spawn_client bottom
wait_for_windows 3
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" settle

# Mod+right-drag the top row's bottom edge to the top of the output, which leaves it at its minimum height.
edge=$(("$(field_of top y)" + "$(field_of top h)" - 2))
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo move 600 "$edge" press 273 move 600 1 release 273 mod none
"$UMBRIEL" settle
top_h=$(field_of top h)
bottom_h=$(field_of bottom h)

"$UMBRIEL" msg window-focus "$(field_of bottom id)" > /dev/null
"$UMBRIEL" msg window-modify-secondary-extent 0.1 > /dev/null
"$UMBRIEL" settle

if (($(field_of bottom h) <= bottom_h)) || (($(field_of top h) != top_h)); then
  echo "expected bottom to grow past $bottom_h with top kept at $top_h: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "the row grew past a row at its minimum"
