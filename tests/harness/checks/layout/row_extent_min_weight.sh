#!/usr/bin/env bash
# Resizing a row in a column with free space keeps the other row's height when the column's weights are so small that
# the resized row stops at its minimum weight.
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

spawn_client upper
wait_for_windows 1
spawn_client lower
wait_for_windows 2
"$UMBRIEL" msg window-consume-left > /dev/null
# Shrinking both rows to the floor leaves the column's weights far below one.
"$UMBRIEL" msg window-modify-secondary-extent -0.9 > /dev/null
"$UMBRIEL" msg window-focus-up > /dev/null
"$UMBRIEL" msg window-modify-secondary-extent -0.9 > /dev/null
"$UMBRIEL" settle

# Mod+right-drag the lower row's bottom edge up to leave free space below it.
bottom=$(("$(field_of lower y)" + "$(field_of lower h)" - 2))
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo move 600 "$bottom" press 273 move 600 "$((bottom - 150))" release 273 \
  mod none
"$UMBRIEL" settle
upper_h=$(field_of upper h)

"$UMBRIEL" msg window-focus-down > /dev/null
"$UMBRIEL" msg window-set-secondary-extent 0.1 > /dev/null
"$UMBRIEL" settle

if [[ $(field_of upper h) != "$upper_h" ]]; then
  echo "expected upper to keep $upper_h: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "the untouched row kept its height"
