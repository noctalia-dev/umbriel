#!/usr/bin/env bash
# A silent workspace move delivers the window to the target workspace while the
# active workspace and the seat stay where they were, the plain move keeps
# following the window, and an emptied source keeps its empty workspace active.
# The named, next, and previous silent forms share one rule.
set -euo pipefail

readonly WORKSPACE_CLIENT="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    return 1
  fi
}

windows() {
  "$UMBRIEL" windows --json
}

id_of() {
  windows | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'
}

# A window reports its workspace by id, so the destination has to be resolved
# through the same client the other workspace checks use.
workspace_id_named() {
  "$WORKSPACE_CLIENT" --all | awk -F'\t' -v name="$1" '$2 == name { print $1; exit }'
}

wait_for_windows() {
  local expected=$1 count=
  for _ in $(seq 40); do
    count=$(windows | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected window(s), got $count"
  return 1
}

wait_for_workspace() {
  local title=$1 expected=$2 actual=
  for _ in $(seq 50); do
    actual=$(windows | jq -r --arg title "$title" '.[] | select(.title == $title) | .workspace')
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on $expected, got $actual"
  return 1
}

# `active` is the seat's own record of which view holds keyboard focus. The
# `focused` field is workspace-local, so several workspaces can report one at
# once and it cannot answer where the seat went.
active_titles() {
  windows | jq -r '[.[] | select(.active) | .title] | sort | join(" ")'
}

wait_for_active() {
  local expected=$1 actual=
  for _ in $(seq 50); do
    actual=$(active_titles)
    if [[ $expected == none ]]; then
      [[ -z $actual ]] && return 0
    else
      [[ $actual == "$expected" ]] && return 0
    fi
    sleep 0.1
  done
  echo "expected the seat on '$expected', got '${actual:-none}'"
  return 1
}

wait_for_active_absent() {
  local title=$1 count=
  for _ in $(seq 50); do
    count=$(windows | jq -r --arg title "$title" '[.[] | select(.active and .title == $title)] | length')
    [[ $count == 0 ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' to stop holding the seat"
  return 1
}

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name'
}

assert_active() {
  local expected=$1 actual
  actual=$(active_workspace)
  if [[ $actual != "$expected" ]]; then
    echo "expected workspace '$expected' to stay active, got '$actual'"
    return 1
  fi
}

# Static workspaces make both destinations exist from the start, so a move can
# never be mistaken for a dynamic-workspace clamp.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output.HEADLESS-1]
workspaces = ["ONE", "TWO"]
EOF
"$UMBRIEL" msg config-reload > /dev/null

one_id=$(workspace_id_named ONE)
two_id=$(workspace_id_named TWO)
if [[ -z $one_id || -z $two_id ]]; then
  echo "static workspaces ONE/TWO did not appear (ONE='$one_id' TWO='$two_id')"
  exit 1
fi
assert_active ONE

spawn_client silent-source
spawn_client silent-anchor
wait_for_windows 2
accepts "window-focus:$(id_of silent-source)"
wait_for_active silent-source

# The window lands on TWO, ONE stays active, and the seat takes the window the
# source workspace hands it instead of travelling with the moved one.
accepts "window-move-to-workspace-silent:TWO"
wait_for_workspace silent-source "$two_id"
assert_active ONE
wait_for_active silent-anchor

# Contrast: the plain form still follows the window onto TWO.
accepts "window-move-to-workspace:TWO"
wait_for_workspace silent-anchor "$two_id"
assert_active TWO

# An emptied source keeps its now-empty workspace active and clears the
# keyboard focus, rather than switching to the destination.
accepts "workspace-switch:ONE"
spawn_client silent-last
wait_for_windows 3
accepts "window-focus:$(id_of silent-last)"
wait_for_active silent-last
accepts "window-move-to-workspace-silent:TWO"
wait_for_workspace silent-last "$two_id"
assert_active ONE
wait_for_active none

# With nothing left to move the action is a no-op, never a workspace switch.
accepts "window-move-to-workspace-silent:TWO"
assert_active ONE

# The adjacent silent forms obey the same rule. TWO now owns all three windows,
# so moving backwards must leave TWO active, keep the seat on TWO, and hand it
# to a window that stayed behind.
accepts "workspace-switch:TWO"
wait_for_active silent-anchor
accepts "window-move-to-workspace-silent-previous"
wait_for_workspace silent-anchor "$one_id"
assert_active TWO
wait_for_active_absent silent-anchor
[[ -n $(active_titles) ]] || { echo "the window left TWO without a surviving seat"; exit 1; }

# Forwards out of the emptied ONE: it stays active and empty, and the window
# still travels.
accepts "workspace-switch:ONE"
wait_for_active silent-anchor
accepts "window-move-to-workspace-silent-next"
wait_for_workspace silent-anchor "$two_id"
assert_active ONE
wait_for_active none

echo "silent workspace moves keep the active workspace and the seat in place"
