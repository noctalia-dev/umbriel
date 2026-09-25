#!/usr/bin/env bash
# A tabbed scrolling column gives every tab the column's box below a bar and draws only the active tab. Focus, the tab
# actions, and a click on the bar each select a tab, and the bar's fill follows. Pixels prove what is drawn, since
# `windows --json` reports layout slots, not visibility.
set -euo pipefail

readonly SHOT="$UMBRIEL_RUNTIME_DIR/tabbed.png"
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272
readonly RED='r > 0.8 && g < 0.2 && b < 0.2'
readonly GREEN='g > 0.8 && r < 0.2 && b < 0.2'
readonly BLUE='b > 0.8 && r < 0.2 && g < 0.2'
# The bar paints in yellow and its active fill in magenta, colours no window or border here uses.
readonly BAR='(r > 0.8 && g > 0.8 && b < 0.2) || (r > 0.8 && b > 0.8 && g < 0.2)'
readonly FILL='r > 0.8 && b > 0.8 && g < 0.2'

printf '\n[layout.scrolling]\ndefault_extent_fraction = 0.5\n' >> "$UMBRIEL_CONFIG"
printf '\n[colors.tab_bar]\nbackground = "#FFFF00FF"\nactive = "#FF00FFFF"\nactive_unfocused = "#FF00FFFF"\n' \
  >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    return 1
  fi
}

spawn_client() {
  FILL_COLOR=$2 RESIZE_FILL_COLOR=$2 "$UMBRIEL_UNMAP_CLIENT" "$1" 400 300 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
}

wait_for_windows() {
  local expected=$1 count=
  for _ in $(seq 50); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected window(s), got $count"
  return 1
}

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

# Waits until `shown` is the focused, visible tab and every other listed title is a hidden tab in the same box.
wait_for_tabs() {
  local shown=$1 windows=
  shift
  for _ in $(seq 50); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e --arg shown "$shown" --args '
      . as $windows
      | (map(select(.title == $shown)) | first) as $s
      | $s != null and $s.tabbed and ($s.tab_hidden | not) and $s.active
        and all($ARGS.positional[]; . as $t
          | ($windows | map(select(.title == $t)) | first) as $h
          | $h != null and $h.tabbed and $h.tab_hidden
            and $h.x == $s.x and $h.y == $s.y and $h.w == $s.w and $h.h == $s.h)
    ' "$@" <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$shown' shown with hidden tabs $* in its box: $windows"
  return 1
}

shoot() {
  "$UMBRIEL" settle
  grim "$SHOT"
}

count() { "$UMBRIEL_PIXEL_PROBE" "$SHOT" count "$1"; }

expect_drawn() {
  local drawn=$1 hidden=$2
  shoot
  if (($(count "$drawn") == 0 || $(count "$hidden") != 0)); then
    echo "expected only the shown tab drawn: shown pixels $(count "$drawn"), hidden pixels $(count "$hidden")"
    return 1
  fi
}

# The active fill sits over tab `index` of `tabs`, and the bar ends above the shown window's top border.
expect_bar() {
  local index=$1 tabs=$2 title=$3
  shoot
  local x y w
  x=$(field_of "$title" x)
  y=$(field_of "$title" y)
  w=$(field_of "$title" w)
  read -r bx by bw bh <<< "$("$UMBRIEL_PIXEL_PROBE" "$SHOT" bbox "$BAR")"
  if ((bw < w || by + bh > y - 2 || bh < 12)); then
    echo "expected a bar above '$title' at x=$x y=$y w=$w, got ${bw}x${bh}+${bx}+${by}"
    return 1
  fi
  read -r fx _ fw _ <<< "$("$UMBRIEL_PIXEL_PROBE" "$SHOT" bbox "$FILL" "${bw}x${bh}+${bx}+${by}")"
  local center=$((fx + fw / 2)) slot_start=$((bx + index * bw / tabs)) slot_end=$((bx + (index + 1) * bw / tabs))
  if ((fw == 0 || center < slot_start || center >= slot_end)); then
    echo "expected the active fill in tab $index of $tabs ($slot_start..$slot_end), got x=$fx w=$fw"
    return 1
  fi
}

spawn_client tab-a 0xFFFF0000
wait_for_windows 1
spawn_client tab-b 0xFF00FF00
wait_for_windows 2

accepts window-consume-left
accepts column-toggle-tabbed
wait_for_tabs tab-b tab-a
expect_drawn "$GREEN" "$RED"
expect_bar 1 2 tab-b

# Stepping up the column is stepping to the previous tab.
accepts window-focus-up
wait_for_tabs tab-a tab-b
expect_drawn "$RED" "$GREEN"
expect_bar 0 2 tab-a

# The tab actions wrap at either end.
accepts column-focus-tab-previous
wait_for_tabs tab-b tab-a
accepts column-focus-tab-next
wait_for_tabs tab-a tab-b

# A click on the right half of the bar selects the second tab.
bar_x=$(($(field_of tab-a x) + 3 * $(field_of tab-a w) / 4))
bar_y=$(($(field_of tab-a y) - 2 - 8 - 12))
"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move "$bar_x" "$bar_y" click "$BTN_LEFT" > /dev/null
wait_for_tabs tab-b tab-a
expect_drawn "$GREEN" "$RED"

echo "tabbed columns hid inactive tabs and switched tabs by focus, action, and click"
