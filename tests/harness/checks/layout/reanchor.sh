#!/usr/bin/env bash
# Closing a window that is off-screen to the left does not shift the visible ones. Removing a column closes the space it held, so everything to its right moves left by its width plus a gap. When that column sat off-screen to the left the user never asked to see that happen, and the window they are actually reading jumps sideways. Re-anchoring on removal prevents that shift. The geometry is pinned by unit tests in tests/unit/scrolling_layout.cpp. What this adds is that it reaches the real close path, measured in Workspace::detachFromLayout before the column goes. The setup is fussy for a reason, and the fussiness is the check. Removal is followed by ensureFocusedVisible, which clamps the scroll into the focused column's visible band; that clamp absorbs the whole shift whenever the scroll is already sitting at a band edge, and every focus-driven route leaves it exactly there. So the earlier versions of this check passed with the compensation ripped out. The scroll has to be strictly inside the band, which means putting it there by hand (layout-scroll-left, or a touchpad swipe in real use), and that is also the only situation in which a user sees the bug.
set -euo pipefail

declare -A CLIENT_PID=()

spawn_titled() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
  CLIENT_PID[$1]=$!
}

window_count() { "$UMBRIEL" windows --json | jq 'length'; }

wait_for_count() {
  for _ in $(seq 80); do
    [[ $(window_count) -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s), have $(window_count)"
  return 1
}

field_of() {
  "$UMBRIEL" windows --json \
    | jq -r --arg t "$1" --arg f "$2" \
      '[.[] | select(.title == $t) | .[$f]] | if length == 1 then .[0] | tostring else "missing" end'
}

# Positions of everything except A, as one comparable string.
survivors() {
  "$UMBRIEL" windows --json | jq -cS '[.[] | select(.title != "A") | {title, x}] | sort_by(.title)'
}

# Spawned one at a time so the columns end up in a known left-to-right order:
# each new window is inserted after the focused one.
for title in A B C D E F; do
  spawn_titled "$title"
  wait_for_count "${#CLIENT_PID[@]}"
done

# Focus C, which scrolls A and B off the left edge and parks the scroll at the
# far edge of C's band.
for _ in 1 2 3; do
  "$UMBRIEL" msg window-focus-left > /dev/null
done
# Then step off that edge, leaving C fully visible but no longer pinned. Without
# this the clamp below would hide whether the compensation ran at all.
for _ in 1 2 3 4; do
  "$UMBRIEL" msg layout-scroll-left > /dev/null
done
"$UMBRIEL" settle

a_x=$(field_of A x)
a_w=$(field_of A w)
c_x=$(field_of C x)
c_w=$(field_of C w)
if [[ $a_x == "missing" || $c_x == "missing" ]]; then
  echo "expected six distinctly titled windows, got: $("$UMBRIEL" windows --json | jq -c '[.[].title]')"
  exit 1
fi

# Preconditions. If either fails the rest proves nothing, so say so rather than
# pass for the wrong reason.
if (( a_x + a_w > 0 )); then
  echo "precondition failed: A is not off-screen left (x=$a_x w=$a_w)"
  echo "  windows: $("$UMBRIEL" windows --json | jq -c '[.[] | {title, x, w}]')"
  exit 1
fi
if (( c_x < 0 || c_x + c_w > 1280 )); then
  echo "precondition failed: focused C is not fully on screen (x=$c_x w=$c_w)"
  echo "  windows: $("$UMBRIEL" windows --json | jq -c '[.[] | {title, x, w}]')"
  exit 1
fi

before=$(survivors)

# Close A without focusing it: focusing would scroll it back into view first.
kill -TERM "${CLIENT_PID[A]}" 2>/dev/null || true
wait_for_count 5
"$UMBRIEL" settle

after=$(survivors)

if [[ $before != "$after" ]]; then
  echo "visible columns moved when an off-screen window closed"
  echo "  before: $before"
  echo "  after:  $after"
  exit 1
fi

echo "closing an off-screen-left window moved nothing"
