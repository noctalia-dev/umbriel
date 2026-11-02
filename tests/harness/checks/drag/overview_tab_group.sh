#!/usr/bin/env bash
# Dragging a tab group's card in the overview moves the whole group, in its order and showing the same tab. A drag the
# overview cancels puts the group back where it was.
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

[animation]
enabled = false

[overview]
zoom = 0.5

[layout.scrolling]
default_extent_fraction = 0.3
center_focused = "never"
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
"$UMBRIEL" msg window-set-primary-extent 0.4 > /dev/null
spawn_client third
wait_for_windows 4
spawn_client other
wait_for_windows 5
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL" settle

tabs() { jq -c 'map(select(.title | startswith("tab-")) | {title, column, row, tab_index, tab_hidden, w}) | sort_by(.title)'; }
before=$("$UMBRIEL" windows --json | tabs)

# The 1280x720 output previews at half size from 320,180, so the group's card sits around 470,370. Closing the
# overview mid-drag cancels the drag.
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
pointer_hold 1280 720 move 470 370 press "$BTN_LEFT" move 600 450 move 735 490 -- release "$BTN_LEFT"
"$UMBRIEL" settle
"$UMBRIEL" msg overview-close > /dev/null
pointer_release
"$UMBRIEL" settle

after=$("$UMBRIEL" windows --json | tabs)
if [[ $after != "$before" ]]; then
  echo "the cancelled drag did not put the group back: $before versus $after"
  exit 1
fi

# Lifted, the group leaves 'third' and 'other' centred in two cards; 735,490 is low on 'other', a row under it.
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 470 370 press "$BTN_LEFT" move 600 450 move 735 490 pause 300 \
  release "$BTN_LEFT"
"$UMBRIEL" settle
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
if ! jq -e '
  (map(select(.title | startswith("tab-"))) | sort_by(.tab_index)) as $tabs
  | (.[] | select(.title == "other")) as $other
  | ($tabs | map(.title)) == ["tab-a", "tab-b", "tab-c"]
    and ($tabs | map(.column) | unique) == [$other.column]
    and ($tabs | map(.row)) == [1, 2, 3]
    and ($tabs | map(.tab_hidden)) == [true, false, true]
' <<< "$windows" > /dev/null; then
  echo "expected tab-a, tab-b, tab-c grouped under 'other', tab-b shown: $windows"
  exit 1
fi

echo "a cancelled overview drag put the group back, and a dropped one moved the whole group"
