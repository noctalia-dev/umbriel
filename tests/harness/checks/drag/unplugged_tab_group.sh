#!/usr/bin/env bash
# harness: outputs=2
# Removing the output a tab group's overview card is dragged on cancels the drag: the whole group follows its
# workspace to the remaining output, still a group showing the dragged tab.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272

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

[layout.scrolling]
default_extent_fraction = 0.5
center_focused = "never"

[animation]
enabled = false

[overview]
zoom = 0.5

[output.HEADLESS-1]
position = [0, 0]

[output.HEADLESS-2]
position = [1280, 0]
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client tab-a
wait_for_windows 1
spawn_client tab-b
wait_for_windows 2
"$UMBRIEL" msg window-consume-left > /dev/null
spawn_client tab-c
wait_for_windows 3
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg column-toggle-tabbed > /dev/null
"$UMBRIEL" msg column-focus-tab 2 > /dev/null
spawn_client other
wait_for_windows 4
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL" settle

# Both outputs are 1280x720 and the windows open on HEADLESS-2, previewed at half size from 1600,180: the group's card
# sits around 1760,370.
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
pointer_hold 2560 720 move 1760 370 press "$BTN_LEFT" move 1980 370 move 2180 370 -- release "$BTN_LEFT"
"$UMBRIEL" settle
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
"$UMBRIEL" settle
pointer_release
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
if ! jq -e '
  (map(select(.title | startswith("tab-"))) | sort_by(.tab_index)) as $tabs
  | (.[] | select(.title == "other")) as $other
  | ($tabs | map(.title)) == ["tab-a", "tab-b", "tab-c"]
    and ($tabs | map(.output) | unique) == ["HEADLESS-1"]
    and ($tabs | map(.column) | unique) == [0]
    and $other.column == 1
    and ($tabs | map(.tab_hidden)) == [true, false, true]
' <<< "$windows" > /dev/null; then
  echo "expected tab-a, tab-b, tab-c grouped before 'other' on HEADLESS-1, tab-b shown: $windows"
  exit 1
fi

echo "the group dragged on a removed output came back whole"
