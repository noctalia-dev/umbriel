#!/usr/bin/env bash
# harness: outputs=3
# An inactive laptop output must not duplicate the focused dock workspace on undock.
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
msg workspace-set-layout master
foot --title=undock-one sleep 120 > /dev/null 2>&1 &
window=
for _ in $(seq 60); do
  window=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "undock-one") | .id')
  [[ -n $window ]] && break
  sleep 0.05
done
[[ -n $window ]]
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
msg output-enable HEADLESS-3
"$UMBRIEL" settle
snapshot=$(state)
if ! jq -e '[.[] | select(.output == "HEADLESS-3" and .name == "1")] as $one
  | ($one | length == 1) and $one[0].occupied and $one[0].active and $one[0].layout == "master"' <<< "$snapshot" > /dev/null; then
  echo "expected one occupied active workspace 1 on the laptop: $snapshot"
  exit 1
fi
msg workspace-switch 1
"$UMBRIEL" windows --json | jq -e --arg id "$window" 'any(.[]; .id == $id and .active)' > /dev/null
"$UMBRIEL" output-create HEADLESS-2 > /dev/null
"$UMBRIEL" settle
snapshot=$(state)
if ! jq -e '[.[] | select(.name == "1")] as $one
  | ($one | length == 1) and $one[0].occupied and $one[0].output == "HEADLESS-2" and $one[0].layout == "master"' <<< "$snapshot" > /dev/null; then
  echo "expected workspace 1 to return to its dock monitor without a duplicate: $snapshot"
  exit 1
fi
echo "undocking retains the occupied workspace 1 without a duplicate"
