#!/usr/bin/env bash
# window-consume-from-left/right pulls the window the neighboring column shows into the focused column without moving
# focus: the row below the focused window when it stands alone, the focused window's tab group when it is a tab. The
# master layout pulls from the neighboring area instead, and dwindle, whose leaves hold one window each, does nothing.
set -euo pipefail

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    return 1
  fi
}

spawn_client() {
  foot --title="harness-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  local want=$1 count=
  for _ in $(seq 60); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$want" ]] && return 0
    sleep 0.1
  done
  echo "expected $want window(s), got $count"
  return 1
}

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "harness-$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

focus_of() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.active) | .title'
}

printf '\n[layout.scrolling]\ndefault_extent_fraction = 0.5\ncenter_focused = "never"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

spawn_client a
wait_for_windows 1
spawn_client b
wait_for_windows 2
spawn_client c
wait_for_windows 3
spawn_client d
wait_for_windows 4
"$UMBRIEL" settle

# Give the first column a different extent, so a pull that took the source's width instead of the focused column's
# would show up below.
"$UMBRIEL" msg window-focus-left > /dev/null
"$UMBRIEL" msg window-focus-left > /dev/null
"$UMBRIEL" msg window-focus-left > /dev/null
"$UMBRIEL" settle
if [[ $(focus_of) != harness-a ]]; then
  echo "expected harness-a focused, got $(focus_of)"
  exit 1
fi
accepts window-set-primary-extent:0.333
"$UMBRIEL" settle

# Focus b, the second column, so both sides have a column to pull from.
"$UMBRIEL" msg window-focus-right > /dev/null
"$UMBRIEL" settle
if [[ $(focus_of) != harness-b ]]; then
  echo "expected harness-b focused, got $(focus_of)"
  exit 1
fi
focused_width=$(field_of b w)
slim_width=$(field_of a w)
if [[ $slim_width == "$focused_width" ]]; then
  echo "expected the first column's extent change to set it apart from $focused_width, got $slim_width"
  exit 1
fi

# From the left: a joins b's column directly below it, takes that column's width, and focus does not follow it.
accepts window-consume-from-left
"$UMBRIEL" settle
if [[ $(field_of a x) != "$(field_of b x)" ]] || (($(field_of a y) <= $(field_of b y))); then
  echo "expected harness-a below harness-b in the same column: $("$UMBRIEL" windows --json)"
  exit 1
fi
if [[ $(field_of b w) != "$focused_width" || $(field_of a w) != "$focused_width" ]]; then
  echo "expected the pulled window to take the focused column's width $focused_width: $("$UMBRIEL" windows --json)"
  exit 1
fi
if [[ $(focus_of) != harness-b ]]; then
  echo "expected focus to stay on harness-b, got $(focus_of)"
  exit 1
fi
if [[ $("$UMBRIEL" windows --json | jq '[.[].x] | unique | length') != 3 ]]; then
  echo "expected three columns after the pull: $("$UMBRIEL" windows --json)"
  exit 1
fi

# From the right: the anchor is the focused row, so c lands between b and a rather than at the column's end.
accepts window-consume-from-right
"$UMBRIEL" settle
if [[ $(field_of c x) != "$(field_of b x)" ]] \
  || (($(field_of c y) <= $(field_of b y))) \
  || (($(field_of c y) >= $(field_of a y))); then
  echo "expected harness-c between harness-b and harness-a: $("$UMBRIEL" windows --json)"
  exit 1
fi
if [[ $(focus_of) != harness-b ]]; then
  echo "expected focus to stay on harness-b, got $(focus_of)"
  exit 1
fi

# Tabbing the column makes its rows one group showing the focused window; the next window pulled in becomes another tab,
# at the end of the group by default, and the shown tab stays.
accepts column-toggle-tabbed
"$UMBRIEL" settle
accepts window-consume-from-right
"$UMBRIEL" settle
windows=$("$UMBRIEL" windows --json)
if ! jq -e '
  length == 4
  and (map(select(.tabbed)) | length) == 4
  and (map(select(.tab_hidden)) | length) == 3
  and (map(select(.title == "harness-b" and .active and (.tab_hidden | not))) | length) == 1
  and (map(select(.title == "harness-d" and .tab_hidden and .tab_index == 3)) | length) == 1
  and ([.[].x] | unique | length) == 1
' <<< "$windows" > /dev/null; then
  echo "expected the pulled window to join the tab group as a hidden last tab: $windows"
  exit 1
fi

# The master layout pulls the neighboring area's window instead of a neighboring column's.
accepts workspace-set-layout:master
"$UMBRIEL" settle
spawn_client e
wait_for_windows 5
"$UMBRIEL" settle
if [[ $(focus_of) != harness-e ]]; then
  echo "expected the new window focused in the master layout, got $(focus_of)"
  exit 1
fi
ex=$(field_of e x)
ey=$(field_of e y)
# The master area is the leftmost one, and holds the window the pull should bring over.
master_title=$("$UMBRIEL" windows --json | jq -r 'sort_by(.x) | .[0].title')
accepts window-consume-from-left
"$UMBRIEL" settle
windows=$("$UMBRIEL" windows --json)
if ! jq -e --arg title "$master_title" --argjson ex "$ex" --argjson ey "$ey" '
  ([.[].x] | unique | length) == 1
  and ((.[] | select(.title == $title) | .y) > $ey)
  and (([.[] | select(.y > $ey) | .y] | min) == (.[] | select(.title == $title) | .y))
' <<< "$windows" > /dev/null; then
  echo "expected $master_title directly below harness-e once the master area emptied: $windows"
  exit 1
fi
if [[ $(focus_of) != harness-e ]]; then
  echo "expected focus to stay on harness-e, got $(focus_of)"
  exit 1
fi

# A dwindle leaf holds one window, so there is nothing to pull and the tree must not move.
accepts workspace-set-layout:dwindle
"$UMBRIEL" settle
before=$("$UMBRIEL" windows --json | jq -c 'sort_by(.title) | map({title, x, y, w, h})')
accepts window-consume-from-left
accepts window-consume-from-right
"$UMBRIEL" settle
after=$("$UMBRIEL" windows --json | jq -c 'sort_by(.title) | map({title, x, y, w, h})')
if [[ $before != "$after" ]]; then
  echo "expected dwindle to leave every window where it was: before=$before after=$after"
  exit 1
fi

echo "consume-from pulled a neighbor under the focused row, into its tab group, and from the master area, and left dwindle alone"
