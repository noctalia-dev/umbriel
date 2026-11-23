#!/usr/bin/env bash
# harness: outputs=3
# A dock can return the same physical monitors on different connector names.
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
[output.HEADLESS-4]
position = [1280, 0]
[output.HEADLESS-5]
position = [2560, 0]
CONFIG
msg config-reload
"$UMBRIEL" output-identity-test HEADLESS-2 HP E27 A > /dev/null
"$UMBRIEL" output-identity-test HEADLESS-3 HP E27 B > /dev/null
msg workspace-switch 7 --output HEADLESS-2
msg workspace-set-layout master
foot --title=identity-a1 sleep 120 > /dev/null 2>&1 &
foot --title=identity-a2 sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  count=$("$UMBRIEL" windows --json | jq '[.[] | select(.title | startswith("identity-a"))] | length')
  [[ $count == 2 ]] && break
  sleep 0.05
done
[[ $count == 2 ]]
msg workspace-switch 8 --output HEADLESS-3
msg workspace-set-layout dwindle
foot --title=identity-b sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  count=$("$UMBRIEL" windows --json | jq '[.[] | select(.title == "identity-b")] | length')
  [[ $count == 1 ]] && break
  sleep 0.05
done
[[ $count == 1 ]]
"$UMBRIEL" settle
before=$("$UMBRIEL" windows --json | jq -c '[.[] | select(.title | startswith("identity-a")) | {title, x: (.x - 1280), y, w, h}] | sort_by(.title)')
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
"$UMBRIEL" output-destroy HEADLESS-3 > /dev/null
"$UMBRIEL" settle
state | jq -e 'any(.[]; .name == "7" and .output == "HEADLESS-1" and .layout == "master")
  and any(.[]; .name == "8" and .output == "HEADLESS-1" and .layout == "dwindle")' > /dev/null

# A different monitor on the old connector must not claim either workspace.
"$UMBRIEL" output-create HEADLESS-2 > /dev/null
"$UMBRIEL" output-identity-test HEADLESS-2 HP E27 C > /dev/null
"$UMBRIEL" settle
state | jq -e 'any(.[]; .name == "7" and .output == "HEADLESS-1")
  and any(.[]; .name == "8" and .output == "HEADLESS-1")' > /dev/null

# The monitors return on new connectors, in reverse order.
"$UMBRIEL" output-create HEADLESS-5 > /dev/null
"$UMBRIEL" output-identity-test HEADLESS-5 HP E27 B > /dev/null
"$UMBRIEL" output-create HEADLESS-4 > /dev/null
"$UMBRIEL" output-identity-test HEADLESS-4 HP E27 A > /dev/null
"$UMBRIEL" settle
snapshot=$(state)
jq -e 'any(.[]; .name == "7" and .output == "HEADLESS-4" and .layout == "master" and .active)
  and any(.[]; .name == "8" and .output == "HEADLESS-5" and .layout == "dwindle" and .active)
  and ([.[] | select(.name == "7" or .name == "8")] | length == 2)' <<< "$snapshot" > /dev/null
a_workspace=$(jq -r '.[] | select(.name == "7" and .output == "HEADLESS-4") | .id' <<< "$snapshot")
b_workspace=$(jq -r '.[] | select(.name == "8" and .output == "HEADLESS-5") | .id' <<< "$snapshot")
"$UMBRIEL" windows --json | jq -e --arg a "$a_workspace" --arg b "$b_workspace" '
  all(.[] | select(.title | startswith("identity-a")); .workspace == $a)
  and any(.[]; .title == "identity-b" and .workspace == $b)' > /dev/null
after=$("$UMBRIEL" windows --json | jq -c '[.[] | select(.title | startswith("identity-a")) | {title, x: (.x - 1280), y, w, h}] | sort_by(.title)')
[[ $before == "$after" ]]
echo "workspaces and layouts follow their physical monitors across connector changes"
