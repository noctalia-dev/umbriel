#!/usr/bin/env bash
# The master layout tabs whole areas: a tabbed stack shows one window in the stack's box while the master area keeps
# its own. Focus entering the stack lands on the shown tab, an emptied tabbed stack stays tabbed for the next window,
# and switching to dwindle, which has no tabs, reveals every window and refuses the toggle.
set -euo pipefail

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    return 1
  fi
}

spawn_client() {
  "$UMBRIEL_UNMAP_CLIENT" "$1" 400 300 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
}

wait_for_windows() {
  local expected=$1 count=
  for _ in $(seq 50); do
    count=$("$UMBRIEL" windows --json | jq '[.[] | select(.w > 0)] | length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected mapped window(s), got $count"
  return 1
}

# Waits for a jq predicate over the window list, where `w(title)` reads one window.
wait_for() {
  local description=$1 predicate=$2 windows=
  for _ in $(seq 50); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e "def w(\$t): map(select(.title == \$t)) | first; $predicate" <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $description: $windows"
  return 1
}

close_window() {
  accepts window-close
  for _ in $(seq 50); do
    grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/$1.log" && return 0
    sleep 0.1
  done
  echo "expected $1 to unmap"
  return 1
}

accepts workspace-set-layout:master
spawn_client master-a
wait_for_windows 1
spawn_client stack-b
wait_for_windows 2
spawn_client stack-c
wait_for_windows 3

# New stack windows land on top, so focus is on stack-c at the top of the stack.
accepts column-toggle-tabbed
wait_for "a tabbed stack showing stack-c beside an untabbed master" '
  (w("stack-c")) as $c | (w("stack-b")) as $b | (w("master-a")) as $m
  | $c.tabbed and ($c.tab_hidden | not) and $b.tabbed and $b.tab_hidden
    and $b.x == $c.x and $b.y == $c.y and $b.w == $c.w and $b.h == $c.h
    and ($m.tabbed | not) and ($m.tab_hidden | not) and $m.x != $c.x'

accepts window-focus-down
wait_for "stack-b shown after stepping down the tabs" '
  (w("stack-b")) as $b | $b.active and ($b.tab_hidden | not) and w("stack-c").tab_hidden'

# Leaving the stack and coming back returns to the tab it shows.
accepts window-focus-left
wait_for "master-a focused" 'w("master-a").active'
accepts window-focus-right
wait_for "focus back on the shown tab stack-b" 'w("stack-b").active and w("stack-c").tab_hidden'

# An emptied tabbed stack keeps its mode for the next window.
close_window stack-b
wait_for "stack-c revealed once stack-b closed" 'w("stack-c").active and (w("stack-c").tab_hidden | not)'
close_window stack-c
spawn_client stack-d
wait_for_windows 2
wait_for "the new stack window arriving as a tab" 'w("stack-d").tabbed and (w("master-a").tabbed | not)'
spawn_client stack-e
wait_for_windows 3
wait_for "the next window joining the tabbed stack" '
  w("stack-e").tabbed and (w("stack-e").tab_hidden | not) and w("stack-d").tab_hidden'

# Dwindle has no multi-window containers: every window shows again, and the toggle is refused with a reason.
accepts workspace-set-layout:dwindle
wait_for "every window untabbed and shown in dwindle" 'all(.[] | select(.w > 0); (.tabbed | not) and (.tab_hidden | not))'
if out=$("$UMBRIEL" msg column-toggle-tabbed 2>&1); then
  echo "expected column-toggle-tabbed to be refused in dwindle"
  exit 1
fi
if [[ $out != *"requires the scrolling or master layout"* ]]; then
  echo "expected a layout reason for the refusal, got: $out"
  exit 1
fi

echo "master areas tabbed independently, kept their mode when emptied, and dwindle refused tabs"
