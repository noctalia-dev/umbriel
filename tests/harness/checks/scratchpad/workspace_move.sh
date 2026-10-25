#!/usr/bin/env bash
# harness: outputs=2
# Workspace move bindings remove the focused scratchpad member, restore its
# saved layout state, and obey follow/silent behavior without moving its peers.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[input.focus]
follows_mouse = false

[input.cursor]
follows_focus = false

[output.HEADLESS-1]
workspaces = ["ONE", "TWO", "THREE"]

[output.HEADLESS-2]
workspaces = ["RIGHT"]
EOF
"$UMBRIEL" msg config-reload > /dev/null

field() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg key "$2" '.[] | select(.title == $title) | .[$key]'
}

wait_field() {
  local actual=
  for _ in $(seq 80); do
    actual=$(field "$1" "$2")
    [[ $actual == "$3" ]] && return 0
    sleep 0.1
  done
  echo "expected $1 $2=$3, got $actual: $("$UMBRIEL" windows --json)"
  return 1
}

workspace_id() {
  "$UMBRIEL" workspaces --json | jq -r --arg name "$1" '.[] | select(.name == $name) | .id'
}

accepts() { "$UMBRIEL" msg "$1" > /dev/null; }

one=$(workspace_id ONE)
two=$(workspace_id TWO)
three=$(workspace_id THREE)
right=$(workspace_id RIGHT)

accepts workspace-switch:ONE/HEADLESS-1
"$CLIENT" scratch-move-anchor > "$UMBRIEL_RUNTIME_DIR/anchor.log" 2>&1 &
wait_field scratch-move-anchor active true
"$CLIENT" scratch-move-member > "$UMBRIEL_RUNTIME_DIR/member.log" 2>&1 &
wait_field scratch-move-member active true
member=$(field scratch-move-member id)

prepare() {
  accepts "window-focus:$member"
  accepts window-move-to-workspace:ONE/HEADLESS-1
  if [[ $(field scratch-move-member floating) != "$1" ]]; then
    accepts window-toggle-floating
  fi
  accepts window-move-to-scratchpad
  wait_field scratch-move-member scratchpad default
  wait_field scratch-move-member workspace ""
  accepts "window-focus:$member"
  wait_field scratch-move-member active true
}

check_move() {
  local action=$1 destination=$2 floating=$3 follow=$4
  prepare "$floating"
  accepts "$action"
  wait_field scratch-move-member workspace "$destination"
  wait_field scratch-move-member scratchpad ""
  wait_field scratch-move-member floating "$floating"
  wait_field scratch-move-anchor workspace "$one"
  wait_field scratch-move-member active "$follow"
  if [[ $follow == false ]]; then
    wait_field scratch-move-anchor active true
    [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .active) | .id') == "$one" ]]
  fi
}

for floating in false true; do
  check_move window-move-to-workspace:2 "$two" "$floating" true
  check_move window-move-to-workspace:RIGHT/HEADLESS-2 "$right" "$floating" true
  check_move window-move-to-workspace-silent:TWO "$two" "$floating" false
  check_move window-move-to-workspace-silent:RIGHT/HEADLESS-2 "$right" "$floating" false
  check_move window-move-to-workspace-next "$two" "$floating" true
  check_move window-move-to-workspace-silent-next "$two" "$floating" false
  # Adjacent moves use the pad's output even when the pointer selects another.
  prepare "$floating"
  accepts workspace-switch:RIGHT/HEADLESS-2
  accepts "window-focus:$member"
  accepts window-move-to-workspace-next
  wait_field scratch-move-member workspace "$two"
  wait_field scratch-move-member scratchpad ""
  wait_field scratch-move-member active true

  prepare "$floating"
  accepts workspace-switch:THREE/HEADLESS-1
  accepts window-move-to-workspace-previous
  wait_field scratch-move-member workspace "$two"
  wait_field scratch-move-member scratchpad ""
  wait_field scratch-move-member floating "$floating"
  wait_field scratch-move-member active true

  prepare "$floating"
  accepts workspace-switch:THREE/HEADLESS-1
  accepts window-move-to-workspace-silent-previous
  wait_field scratch-move-member workspace "$two"
  wait_field scratch-move-member scratchpad ""
  wait_field scratch-move-member floating "$floating"
  wait_field scratch-move-member active false
  [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .active) | .id') == "$three" ]]
done

# A rejected selector must leave scratchpad membership intact.
prepare false
if "$UMBRIEL" msg window-move-to-workspace:MISSING > "$UMBRIEL_RUNTIME_DIR/rejected.log" 2>&1; then
  echo "an unknown workspace was accepted"
  exit 1
fi
wait_field scratch-move-member scratchpad default
wait_field scratch-move-member workspace ""
wait_field scratch-move-member active true

# Returning to the currently active workspace still removes membership. A peer
# stays in the pad, and later toggles cannot pull the moved member back in.
"$CLIENT" scratch-move-peer > "$UMBRIEL_RUNTIME_DIR/peer.log" 2>&1 &
wait_field scratch-move-peer active true
accepts window-move-to-scratchpad
wait_field scratch-move-peer scratchpad default
accepts "window-focus:$member"
accepts window-move-to-workspace-silent:TWO
wait_field scratch-move-member workspace "$two"
wait_field scratch-move-member scratchpad ""
wait_field scratch-move-member active false
wait_field scratch-move-peer scratchpad default
wait_field scratch-move-peer active true
prepare false
accepts window-move-to-workspace:ONE
wait_field scratch-move-member workspace "$one"
wait_field scratch-move-member scratchpad ""
wait_field scratch-move-member active true
wait_field scratch-move-peer scratchpad default
accepts scratchpad-toggle
accepts scratchpad-toggle
wait_field scratch-move-member workspace "$one"
wait_field scratch-move-peer scratchpad default

echo "workspace moves release scratchpad membership and preserve follow/silent behavior and saved layout state"
