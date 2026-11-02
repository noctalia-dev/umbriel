#!/usr/bin/env bash
# A tab group shows one window in its box below its bar and hides the rest. Tab actions select, reorder, move tabs out
# of and into the group, hide its bar, and untab; hidden tabs stay hidden across a workspace switch; a closed tab hands
# the group to its predecessor; and a layout without columns refuses tabs.
set -euo pipefail

readonly RESERVE=32 # [appearance.tab_bar] height 24 plus layout gap 8

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

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

# Waits until exactly `$1` is the shown, active tab and every other window is a hidden tab of the same box.
wait_for_shown() {
  local shown=$1 windows=
  for _ in $(seq 50); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e --arg shown "$shown" '
      (map(select(.tabbed)) | length) == length
      and (map(select(.tab_hidden)) | length) == length - 1
      and (map(select(.title == $shown and (.tab_hidden | not) and .active)) | length) == 1
      and (map([.x, .y, .w, .h]) | unique | length) == 1
    ' <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$shown' to be the only shown tab: $windows"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^tab-"
default_scrolling_column = "tabs"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client tab-a
wait_for_windows 1
spawn_client tab-b
wait_for_windows 2
spawn_client tab-c
wait_for_windows 3
"$UMBRIEL" settle
stacked_y=$(field_of tab-a y)
focused=$("$UMBRIEL" windows --json | jq -r '.[] | select(.active) | .title')

accepts column-toggle-tabbed
"$UMBRIEL" settle
wait_for_shown "$focused"
tabbed_y=$(field_of tab-a y)
if (( tabbed_y != stacked_y + RESERVE )); then
  echo "expected the tabs to start $RESERVE pixels below the stacked top $stacked_y, got $tabbed_y"
  exit 1
fi
if [[ $("$UMBRIEL" windows | grep -c $'\ttab-[abc]\t\[tab ') != 3 ]]; then
  echo "expected the plain listing to mark all three windows as tabs: $("$UMBRIEL" windows)"
  exit 1
fi

accepts column-focus-tab 1
wait_for_shown tab-a
accepts column-focus-tab -1
wait_for_shown tab-c
accepts column-focus-tab-next
wait_for_shown tab-a
accepts column-focus-tab-previous
wait_for_shown tab-c
accepts column-focus-tab-previous
wait_for_shown tab-b

accepts column-move-tab-previous
"$UMBRIEL" settle
if [[ $(field_of tab-b tab_index) != 0 ]]; then
  echo "expected column-move-tab-previous to make tab-b the first tab: $("$UMBRIEL" windows --json)"
  exit 1
fi
wait_for_shown tab-b

# A workspace switch neither reveals the hidden tabs nor changes which one shows.
accepts workspace-switch 2
"$UMBRIEL" settle
accepts workspace-switch 1
"$UMBRIEL" settle
wait_for_shown tab-b

# Closing the shown tab shows the tab before it; tab-b is first, so the next one takes its place.
accepts window-close
wait_for_windows 2
"$UMBRIEL" settle
wait_for_shown tab-a

# A tab group is one row of its column: a tab moving down leaves it for a row of its own below, and moving back up
# joins it again as its shown tab.
accepts window-move-down
"$UMBRIEL" settle
if [[ $(field_of tab-a tabbed) != false ]] || (( $(field_of tab-a y) <= $(field_of tab-c y) )); then
  echo "expected tab-a to stand alone below the group: $("$UMBRIEL" windows --json)"
  exit 1
fi
accepts window-move-up
"$UMBRIEL" settle
wait_for_shown tab-a

# A hidden bar gives its space to the tabs, which keep working; showing it takes the space back.
accepts column-toggle-tab-bar
"$UMBRIEL" settle
if (( $(field_of tab-a y) != stacked_y )); then
  echo "expected a hidden bar to give the tabs its space, top $stacked_y, got $(field_of tab-a y)"
  exit 1
fi
accepts column-focus-tab-next
wait_for_shown tab-c
accepts column-show-tab-bar
"$UMBRIEL" settle
if (( $(field_of tab-c y) != tabbed_y )); then
  echo "expected a shown bar to take its space back, top $tabbed_y, got $(field_of tab-c y)"
  exit 1
fi

accepts column-set-display normal
"$UMBRIEL" settle
if [[ $("$UMBRIEL" windows --json | jq '[.[] | select(.tabbed or .tab_hidden)] | length') != 0 ]]; then
  echo "expected column-set-display normal to leave no tabs: $("$UMBRIEL" windows --json)"
  exit 1
fi

accepts workspace-set-layout dwindle
"$UMBRIEL" settle
if out=$("$UMBRIEL" msg column-toggle-tabbed 2>&1) || [[ $out != *"scrolling or master"* ]]; then
  echo "expected dwindle to refuse tabs, got: $out"
  exit 1
fi

echo "tabbed columns selected, reordered, hid, closed, and untabbed their windows"
