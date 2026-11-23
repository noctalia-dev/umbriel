#!/usr/bin/env bash
# harness: outputs=3
# A conflicting laptop workspace with a window must not be merged into the evacuated workspace.
set -euo pipefail

msg() { "$UMBRIEL" msg "$@" > /dev/null; }
state() { "$UMBRIEL" workspaces --json; }
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[workspaces]
global_numbers = true
[output.HEADLESS-1]
position = [0, 0]
[output.HEADLESS-2]
position = [1280, 0]
[output.HEADLESS-3]
position = [2560, 0]
CONFIG
msg config-reload
msg workspace-switch 1 --output HEADLESS-3
msg output-disable HEADLESS-3
msg workspace-switch 1 --output HEADLESS-2
foot --title=dock-window sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  dock=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "dock-window") | .id')
  [[ -n $dock ]] && break
  sleep 0.05
done
[[ -n $dock ]]
msg output-enable HEADLESS-3
msg workspace-switch --name 1 --output HEADLESS-3
foot --title=laptop-window sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  laptop=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "laptop-window") | .id')
  [[ -n $laptop ]] && break
  sleep 0.05
done
[[ -n $laptop ]]
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
"$UMBRIEL" settle
snapshot=$(state)
windows=$("$UMBRIEL" windows --json)
dock_workspace=$(jq -r --arg id "$dock" '.[] | select(.id == $id) | .workspace' <<< "$windows")
laptop_workspace=$(jq -r --arg id "$laptop" '.[] | select(.id == $id) | .workspace' <<< "$windows")
if ! jq -e --arg dock "$dock_workspace" --arg laptop "$laptop_workspace" '
  $dock != $laptop
  and ([.[] | select(.name == "1")] | length == 1)
  and any(.[]; .id == $dock and .name == "1" and .occupied)
  and any(.[]; .id == $laptop and .name != "1" and .occupied)
' <<< "$snapshot" > /dev/null; then
  echo "expected separate workspaces with unique numbers: $snapshot; windows: $windows"
  exit 1
fi
"$UMBRIEL" output-create HEADLESS-2 > /dev/null
"$UMBRIEL" settle
snapshot=$(state)
windows=$("$UMBRIEL" windows --json)
dock_workspace=$(jq -r --arg id "$dock" '.[] | select(.id == $id) | .workspace' <<< "$windows")
laptop_workspace=$(jq -r --arg id "$laptop" '.[] | select(.id == $id) | .workspace' <<< "$windows")
jq -e --arg dock "$dock_workspace" --arg laptop "$laptop_workspace" '
  any(.[]; .id == $dock and .name == "1" and .output == "HEADLESS-2")
  and any(.[]; .id == $laptop and .output == "HEADLESS-3" and .name != "1")
' <<< "$snapshot" > /dev/null
echo "occupied local and evacuated workspaces stay separate during undock and reconnect"
