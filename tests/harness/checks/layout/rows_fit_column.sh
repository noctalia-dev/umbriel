#!/usr/bin/env bash
# Windows joining a column whose other row was set to full height still fit on the output: each takes its minimum from
# the full row instead of stacking past the column's end.
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

# Title bars raise every window's minimum height, so the joining windows cannot all fit at their minimums below a full
# row.
printf '\n[appearance.title_bar]\nmode = "on"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

spawn_client full
wait_for_windows 1
spawn_client below
wait_for_windows 2
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg window-focus-up > /dev/null
"$UMBRIEL" msg window-set-secondary-extent 1.0 > /dev/null

for count in 3 4 5; do
  spawn_client "joined-$count"
  wait_for_windows "$count"
  "$UMBRIEL" msg window-consume-left > /dev/null
done
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
if ! jq -e 'all(.[]; .y >= 0 and .y + .h <= 720)' <<< "$windows" > /dev/null; then
  echo "expected every window inside the 720 px output: $windows"
  exit 1
fi

echo "every row stayed inside the column"
