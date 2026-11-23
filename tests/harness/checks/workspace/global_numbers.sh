#!/usr/bin/env bash
# harness: outputs=2
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
workspaces = 9
[output.HEADLESS-2]
position = [1280, 0]
workspaces = 9
[[workspace]]
index = 7
layout.mode = "master"
[[workspace]]
index = 20
layout.mode = "dwindle"
[[window_rule]]
match.title = "^global-rule$"
default_workspace = 6
CONFIG
msg config-reload
msg workspace-switch 20 --output HEADLESS-1
state | jq -e 'any(.[]; .name == "20" and .layout == "dwindle")' > /dev/null
msg workspace-switch 8 --output HEADLESS-2
foot --title=global-eight sleep 120 > /dev/null 2>&1 &
window=
for _ in $(seq 60); do
  window=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "global-eight") | .id')
  [[ -n $window ]] && break
  sleep 0.05
done
[[ -n $window ]]
msg workspace-switch 1 --output HEADLESS-1
msg workspace-switch 8
"$UMBRIEL" settle
state | jq -e 'any(.[]; .focused and .output == "HEADLESS-2" and .name == "8")
  and ([.[] | select(.name == "8")] | length == 1)' > /dev/null
msg workspace-switch 9
msg workspace-switch 8
"$UMBRIEL" settle
state | jq -e 'all(.[]; .name != "9")' > /dev/null
msg workspace-switch 1 --output HEADLESS-1
msg workspace-switch 9
"$UMBRIEL" settle
state | jq -e 'any(.[]; .focused and .output == "HEADLESS-1" and .name == "9")' > /dev/null
msg workspace-switch 8
msg window-move-to-workspace-silent 7
"$UMBRIEL" settle
destination=$("$UMBRIEL" windows --json | jq -r --arg id "$window" '.[] | select(.id == $id) | .workspace')
state | jq -e --arg id "$destination" '
  any(.[]; .focused and .output == "HEADLESS-2" and .name == "8")
  and any(.[]; .id == $id and .output == "HEADLESS-2" and .name == "7" and .layout == "master")' > /dev/null
foot --title=global-rule sleep 120 > /dev/null 2>&1 &
rule_window=
for _ in $(seq 60); do
  rule_window=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "global-rule") | .workspace')
  [[ -n $rule_window ]] && break
  sleep 0.05
done
[[ -n $rule_window ]]
state | jq -e --arg id "$rule_window" 'any(.[]; .id == $id and .name == "6")' > /dev/null
msg workspace-move-to-output HEADLESS-1 7
"$UMBRIEL" settle
state | jq -e --arg id "$destination" 'any(.[]; .id == $id and .name == "7" and .output == "HEADLESS-1")' > /dev/null
msg workspace-move-to-output HEADLESS-2 7
"$UMBRIEL" settle
state | jq -e --arg id "$destination" 'any(.[]; .id == $id and .name == "7" and .output == "HEADLESS-2")' > /dev/null
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
"$UMBRIEL" settle
msg workspace-switch 7
state | jq -e 'any(.[]; .focused and .output == "HEADLESS-1" and .name == "7")
  and ([.[] | select(.name == "7")] | length == 1)' > /dev/null
"$UMBRIEL" output-create HEADLESS-2 > /dev/null
"$UMBRIEL" settle
destination=$("$UMBRIEL" windows --json | jq -r --arg id "$window" '.[] | select(.id == $id) | .workspace')
state | jq -e --arg id "$destination" '([.[] | select(.name == "7")] | length == 1)
  and any(.[]; .id == $id and .name == "7" and .output == "HEADLESS-2")' > /dev/null
msg workspace-switch 7
state | jq -e 'any(.[]; .focused and .name == "7")' > /dev/null
echo "global workspace numbers follow their workspace and can be reclaimed"
