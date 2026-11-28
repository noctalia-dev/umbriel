#!/usr/bin/env bash
# Resizing a row in a column that leaves free space below its rows trades with that space, growing or shrinking, and
# keeps the other rows' heights.
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

height_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .h'
}

spawn_client upper
wait_for_windows 1
spawn_client lower
wait_for_windows 2
"$UMBRIEL" msg window-consume-left > /dev/null
# A tab group shrunk to a third and untabbed leaves both rows sharing that third, with the rest of the column free.
"$UMBRIEL" msg column-toggle-tabbed > /dev/null
"$UMBRIEL" msg window-set-secondary-extent 0.333 > /dev/null
"$UMBRIEL" msg column-toggle-tabbed > /dev/null
"$UMBRIEL" msg window-focus-up > /dev/null
"$UMBRIEL" settle
lower_h=$(height_of lower)

# Row sizes round to whole pixels, so the untouched row may move by one.
for fraction in 0.5 0.2; do
  upper_h=$(height_of upper)
  "$UMBRIEL" msg window-set-secondary-extent "$fraction" > /dev/null
  "$UMBRIEL" settle
  windows=$("$UMBRIEL" windows --json)
  if ! jq -e --argjson upper_h "$upper_h" --argjson lower_h "$lower_h" '
    (.[] | select(.title == "upper")) as $upper
    | (.[] | select(.title == "lower")) as $lower
    | $upper.h != $upper_h and ($lower.h - $lower_h | fabs) <= 1
  ' <<< "$windows" > /dev/null; then
    echo "expected upper to resize from $upper_h to $fraction with lower kept at $lower_h: $windows"
    exit 1
  fi
done

echo "the row traded with the free space"
