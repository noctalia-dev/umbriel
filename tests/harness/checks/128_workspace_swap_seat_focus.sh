#!/usr/bin/env bash
# harness: outputs=2
# Swapping the active workspaces of two outputs leaves the seat on the output the swap was invoked from. The focus
# ring, xdg activation and keyboard focus all follow one seat-global view, so when the window holding them is the one
# that travels, following its identity strands the seat on the far output while the pointer and the near workspace's
# remembered focus stay behind. Both notions of focus have to keep naming the same window on the pointer's output,
# and they have to survive swapping back, which exercises the same redirection from the other side.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"
# Both outputs are 1280x720 side by side, so pointer coordinates are layout-global.
readonly LAYOUT_W=2560
readonly LAYOUT_H=720

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    exit 1
  fi
}

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" \
    '.[] | select(.title == $title) | .[$field]'
}

wait_for_windows() {
  local expected=$1 count=
  for _ in $(seq 40); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    if [[ $count == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $expected window(s), got $count"
  exit 1
}

wait_for_workspace() {
  local title=$1 expected=$2 actual=
  for _ in $(seq 50); do
    actual=$(field_of "$title" workspace)
    if [[ $actual == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$title' on $expected, got $actual"
  exit 1
}

workspace_id_named() {
  "$WORKSPACE" --all | awk -F'\t' -v name="$1" '$2 == name { print $1; exit }'
}

# The single window holding seat-global activation, as "<workspace> <title>".
# Reports the count instead when the compositor left none or several activated,
# which is itself a failure this check must catch.
active_window() {
  "$UMBRIEL" windows --json \
    | jq -r '[.[] | select(.active)] | if length == 1 then "\(.[0].workspace) \(.[0].title)" else "count=\(length)" end'
}

# Asserts that exactly `title` on `workspace` holds seat activation, and that the
# same window is its workspace's remembered focus, so the ring the user sees and
# the focus every pointer-resolved action reaches cannot disagree.
assert_seat_on() {
  local workspace=$1 title=$2 want="$1 $2" actual=
  for _ in $(seq 50); do
    actual=$(active_window)
    if [[ $actual == "$want" ]]; then
      break
    fi
    sleep 0.05
  done
  if [[ $actual != "$want" ]]; then
    echo "expected the activated window to be '$want', got '$actual'"
    exit 1
  fi
  local focused=
  focused=$(field_of "$title" focused)
  if [[ $focused != true ]]; then
    echo "'$title' holds seat activation but is not its workspace's remembered focus (focused=$focused)"
    exit 1
  fi
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout.scrolling]
default_extent_fraction = 0.5

[animation]
enabled = false

# The pointer must stay put across the swap: a follow-warp would carry it to the
# far output and hide the disagreement this check is about.
[input.cursor]
follows_focus = false

# Hover focus would reconcile the seat on the next motion, so the swap has to
# leave a correct state on its own.
[input.focus]
follows_mouse = false

[output.HEADLESS-1]
position = [0, 0]
workspaces = ["LEFT"]

[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["RIGHT"]
EOF
"$UMBRIEL" msg config-reload > /dev/null

left=$(workspace_id_named LEFT)
right=$(workspace_id_named RIGHT)
if [[ -z $left || -z $right ]]; then
  echo "named workspaces missing: left='$left' right='$right'"
  exit 1
fi

accepts "workspace-switch:LEFT/HEADLESS-1"
spawn_client home
wait_for_windows 1
wait_for_workspace home "$left"

accepts "workspace-switch:RIGHT/HEADLESS-2"
spawn_client away
wait_for_windows 2
wait_for_workspace away "$right"

# The swap resolves its source output from the pointer, so park it over the left
# output and give that output's window the seat.
accepts "workspace-switch:LEFT/HEADLESS-1"
"$POINTER" "$LAYOUT_W" "$LAYOUT_H" move 640 360
accepts "window-focus:$(field_of home id)"
assert_seat_on "$left" home

# 'home' travels to the right output. The seat belongs to the pointer's output,
# so it has to land on what arrived here rather than chase the window that left.
accepts workspace-swap-active-output-next
wait_for_workspace home "$right"
wait_for_workspace away "$left"
assert_seat_on "$left" away

# Swapping back exercises the same redirection with the roles reversed: 'away'
# now holds the seat and is the one that leaves.
accepts workspace-swap-active-output-next
wait_for_workspace home "$left"
wait_for_workspace away "$right"
assert_seat_on "$left" home

echo "active workspace swap kept seat activation and remembered focus together on the pointer's output, in both directions"
