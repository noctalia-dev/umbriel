#!/usr/bin/env bash
# A tab maximized to edges covers the output only while it is shown; the group's other tabs keep the group's place and
# width.
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

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client tab-a
wait_for_windows 1
spawn_client tab-b
wait_for_windows 2
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg column-toggle-tabbed > /dev/null
"$UMBRIEL" msg column-focus-tab 1 > /dev/null
"$UMBRIEL" msg window-set-primary-extent 0.5 > /dev/null
"$UMBRIEL" settle

box() { "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title) | [.x, .y, .w, .h]'; }
group=$(box tab-a)

"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
"$UMBRIEL" settle
maximized=$(box tab-a)

"$UMBRIEL" msg column-focus-tab-next > /dev/null
"$UMBRIEL" settle
if [[ $(box tab-b) != "$group" ]]; then
  echo "expected tab-b in the group's box $group, got $(box tab-b)"
  exit 1
fi

"$UMBRIEL" msg column-focus-tab-previous > /dev/null
"$UMBRIEL" settle
if [[ $(box tab-a) != "$maximized" ]]; then
  echo "expected tab-a maximized again in $maximized, got $(box tab-a)"
  exit 1
fi

echo "the other tab kept the group's box, and the maximized tab covered the output again"
