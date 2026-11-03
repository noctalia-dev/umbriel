#!/usr/bin/env bash
# A bar with more tabs than max_tabs shows a scrolled run of them that follows the active tab, its end buttons cycle
# through every tab, wrapping at the ends, and a click selects the tab drawn under it, not the tab at that position among
# all of them.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BORDER=2
readonly GAP=8
readonly BAR=24
readonly SLOTS=2
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

accepts() {
  if ! out=$("$UMBRIEL" msg "$@" 2>&1); then
    echo "expected '$*' to be accepted, got: $out"
    return 1
  fi
}

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  local expected=$1 count=
  for _ in $(seq 60); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected window(s), got $count"
  return 1
}

wait_for_shown() {
  local shown=$1 windows=
  for _ in $(seq 50); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e --arg shown "$shown" '
      map(select(.title == $shown and .tabbed and (.tab_hidden | not) and .active)) | length == 1
    ' <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$shown' to be the shown tab: $windows"
  return 1
}

# The centre of a part of the scrolling bar: `back`, `forward`, or visible slot N. The bar has more tabs than slots, so
# a square button sits at each end and, with tab_gap 0, the slots split the rest evenly.
part_center() {
  "$UMBRIEL" windows --json | jq -r --arg part "$1" --argjson count "$SLOTS" --argjson border "$BORDER" \
    --argjson gap "$GAP" --argjson bar "$BAR" '
    map(select(.tabbed and (.tab_hidden | not)))[0]
    | (.x - $border) as $left | (.w + 2 * $border) as $width
    | (if $part == "back" then $left + $bar / 2
       elif $part == "forward" then $left + $width - $bar / 2
       else $left + $bar + (($width - 2 * $bar) * (2 * ($part | tonumber) + 1) / (2 * $count)) end | floor) as $x
    | "\($x) \(.y - $border - $gap - ($bar / 2 | floor))"
  '
}

click_part() {
  local x y
  read -r x y <<< "$(part_center "$1")"
  pointer move "$x" "$y" click "$BTN_LEFT"
  "$UMBRIEL" settle
}

cat >> "$UMBRIEL_CONFIG" <<EOF

[layout.tabs]
default_display = "tabbed"

[appearance.tab_bar]
tab_gap = 0
max_tabs = $SLOTS

[[window_rule]]
match.title = "^tab-"
default_scrolling_column = "tabs"
EOF
"$UMBRIEL" msg config-reload > /dev/null

count=0
for title in tab-a tab-b tab-c tab-d; do
  spawn_client "$title"
  count=$((count + 1))
  wait_for_windows "$count"
done
"$UMBRIEL" settle

# The strip follows the active tab to the end: tab-c and tab-d show, so the first slot is tab-c.
accepts column-focus-tab -1
wait_for_shown tab-d
click_part 0
wait_for_shown tab-c

# Back to the start: tab-a and tab-b show.
accepts column-focus-tab 1
wait_for_shown tab-a
click_part 1
wait_for_shown tab-b

# One step past the strip's end scrolls it by one slot: tab-b and tab-c show, and tab-c is now the second slot.
accepts column-focus-tab-next
wait_for_shown tab-c
click_part 0
wait_for_shown tab-b

# The buttons cycle through every tab, and the strip follows: from tab-b on to tab-d, then round to tab-a.
click_part forward
wait_for_shown tab-c
click_part forward
wait_for_shown tab-d
click_part forward
wait_for_shown tab-a
# The strip came back to the start with it, so its first slot is tab-a and its second tab-b.
click_part 1
wait_for_shown tab-b
# Back past the start wraps to the last tab.
click_part back
wait_for_shown tab-a
click_part back
wait_for_shown tab-d

echo "a scrolled tab strip followed the active tab, cycled with its buttons, and mapped clicks to the tabs it drew"
