#!/usr/bin/env bash
# A Mod+drag on a tabbed window carries its whole group: dropped past the next column, every tab arrives in one
# column in its old order, with the same tab shown and the column's width kept.
set -euo pipefail

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
"$UMBRIEL" msg window-set-primary-extent 0.4 > /dev/null
spawn_client other
wait_for_windows 4
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
read -r x y width <<< "$(jq -r '.[] | select(.title == "tab-b") | "\(.x + (.w / 2 | floor)) \(.y + (.h / 2 | floor)) \(.w)"' <<< "$windows")"
read -r drop_x drop_y <<< "$(jq -r '.[] | select(.title == "other") | "\(.x + .w - 20) \(.y + (.h / 2 | floor))"' <<< "$windows")"

"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo move "$x" "$y" press "$BTN_LEFT" move "$((x + 40))" "$y" move "$drop_x" "$drop_y" \
  release "$BTN_LEFT" mod none
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
if ! jq -e --argjson width "$width" '
  (map(select(.title | startswith("tab-"))) | sort_by(.tab_index)) as $tabs
  | (.[] | select(.title == "other")) as $other
  | ($tabs | map(.title)) == ["tab-a", "tab-b", "tab-c"]
    and ($tabs | map(.tab_index)) == [0, 1, 2]
    and ($tabs | map(.x) | unique | length) == 1
    and $tabs[0].x > $other.x
    and $tabs[1].w == $width
    and $tabs[1].active and ($tabs[1].tab_hidden | not)
    and $tabs[0].tab_hidden and $tabs[2].tab_hidden
' <<< "$windows" > /dev/null; then
  echo "expected the whole tab group, tab-b shown, after 'other' at width $width: $windows"
  exit 1
fi

echo "Mod+drag moved the tab group as one"
