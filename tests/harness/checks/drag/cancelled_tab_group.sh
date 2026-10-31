#!/usr/bin/env bash
# Opening the overview cancels a Mod+drag of a tab before it drops anywhere: the whole group, the dragged tab with it,
# goes back where it was, showing the dragged tab.
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

read -r x y <<< "$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "tab-b") | "\(.x + 300) \(.y + 300)"')"
pointer_hold 1280 720 mod logo move "$x" "$y" press "$BTN_LEFT" move "$((x + 40))" "$y" -- release "$BTN_LEFT" mod none
"$UMBRIEL" settle
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" msg overview-close > /dev/null
pointer_release
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
if ! jq -e '
  (map(select(.title | startswith("tab-"))) | sort_by(.tab_index)) as $tabs
  | (.[] | select(.title == "other")) as $other
  | ($tabs | map(.title)) == ["tab-a", "tab-b", "tab-c"]
    and ($tabs | map(.tab_index)) == [0, 1, 2]
    and ($tabs | map(.column) | unique) == [0]
    and $tabs[0].x < $other.x
    and ($tabs | map(.tab_hidden)) == [true, false, true]
' <<< "$windows" > /dev/null; then
  echo "expected tab-a, tab-b, tab-c grouped in place before 'other', tab-b shown: $windows"
  exit 1
fi

echo "the cancelled drag put the whole group back"
