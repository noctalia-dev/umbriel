#!/usr/bin/env bash
# A table-form keybind may leave its current submap or enter a nested one after its primary action. The primary action
# runs first, and omitting the optional transition keeps the existing persistent-submap behavior.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
readonly KEY_S=31
readonly KEY_1=2
readonly KEY_2=3
readonly KEY_3=4
readonly KEY_4=5

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[keybinds]
"Mod+S" = { action = "submap:outer", repeat = false }
"submap[outer],1" = { action = "workspace-set-layout:dwindle", submap = "reset" }
"submap[outer],2" = { action = "workspace-set-layout:master", submap = "inner" }
"submap[outer],3" = { action = "submap:inner", submap = "reset" }
"submap[outer],4" = { action = "workspace-set-layout:master", repeat = true }
EOF
"$UMBRIEL" msg config-reload > /dev/null

active_submap() {
  "$UMBRIEL" submap --json | jq -r '. // "default"'
}

focused_layout() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.focused) | .layout'
}

wait_for_value() {
  local description=$1 expected=$2 command=$3 actual=
  for _ in $(seq 40); do
    actual=$("$command")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $description '$expected', got '$actual'"
  return 1
}

enter_outer() {
  "$POINTER" 1280 720 mod logo tap "$KEY_S" mod none
  wait_for_value "active submap" outer active_submap
}

enter_outer
"$POINTER" 1280 720 tap "$KEY_4"
wait_for_value "focused layout" master focused_layout
wait_for_value "persistent active submap" outer active_submap

"$POINTER" 1280 720 tap "$KEY_1"
wait_for_value "focused layout" dwindle focused_layout
wait_for_value "active submap" default active_submap

enter_outer
"$POINTER" 1280 720 tap "$KEY_2"
wait_for_value "focused layout" master focused_layout
wait_for_value "active submap" inner active_submap
"$UMBRIEL" msg submap:reset > /dev/null
wait_for_value "revealed parent submap" outer active_submap

# Action first pushes inner, then the post-action reset pops it. Reversing that
# order would pop outer first and leave inner active.
"$POINTER" 1280 720 tap "$KEY_3"
wait_for_value "action-then-transition submap" outer active_submap

"$UMBRIEL" msg submap:reset > /dev/null
wait_for_value "final active submap" default active_submap

echo "keybind actions reset or enter submaps after dispatch while preserving nested stack order"
