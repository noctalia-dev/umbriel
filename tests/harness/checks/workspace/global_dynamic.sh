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
global_dynamic = true
[output.HEADLESS-1]
position = [0, 0]
[output.HEADLESS-2]
position = [1280, 0]
CONFIG
msg config-reload
msg workspace-switch 8 --output HEADLESS-2
foot --title=dynamic-right sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  right=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "dynamic-right") | .id')
  [[ -n $right ]] && break
  sleep 0.05
done
[[ -n $right ]]
msg workspace-switch 7 --output HEADLESS-1
foot --title=dynamic-left sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  left=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "dynamic-left") | .id')
  [[ -n $left ]] && break
  sleep 0.05
done
[[ -n $left ]]
"$UMBRIEL" settle
# The next action reaches a new numbered empty workspace; activating it never
# duplicates a number already in use on the other output.
before=$(state)
msg workspace-next
"$UMBRIEL" settle
after=$(state)
jq -e --argjson before "$before" '
  [.[] | select(.focused)] as $focused
  | ($focused | length == 1) and $focused[0].output == "HEADLESS-1"
    and ($focused[0].occupied | not) and $focused[0].name == "9"
    and all($before[] | select(.output == "HEADLESS-2"); .name != $focused[0].name)
' <<< "$after" > /dev/null
foot --title=dynamic-new sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  new=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "dynamic-new") | .id')
  [[ -n $new ]] && break
  sleep 0.05
done
[[ -n $new ]]
"$UMBRIEL" settle
new_workspace=$("$UMBRIEL" windows --json | jq -r --arg id "$new" '.[] | select(.id == $id) | .workspace')
state | jq -e --arg id "$new_workspace" '
  any(.[]; .id == $id and .occupied and .output == "HEADLESS-1")
  and any(.[]; .output == "HEADLESS-1" and .name == "+" and (.active | not))' > /dev/null
# Swipes can reach the same empty slot and assign the next globally free number.
"$UMBRIEL" swipe-test begin 1000 > /dev/null
"$UMBRIEL" swipe-test update 1100 0 -240 > /dev/null
"$UMBRIEL" swipe-test end 1600 0 > /dev/null
"$UMBRIEL" settle
state | jq -e 'any(.[]; .focused and .output == "HEADLESS-1" and .name == "10" and (.occupied | not))' > /dev/null
msg workspace-switch 7
"$UMBRIEL" settle
state | jq -e 'all(.[]; .name != "10")
  and ([.[] | select(.output == "HEADLESS-1" and .name == "+")] | length == 1)' > /dev/null
"$UMBRIEL" config set workspaces.global_dynamic false > /dev/null
"$UMBRIEL" settle
state | jq -e 'all(.[]; .name != "+")' > /dev/null
"$UMBRIEL" config unset workspaces.global_dynamic > /dev/null
"$UMBRIEL" settle
state | jq -e 'any(.[]; .output == "HEADLESS-1" and .name == "+" and (.active | not))' > /dev/null
# Dropping or silently moving a window into the empty slot assigns a number,
# even when the slot is not activated first.
msg window-move-to-workspace-silent --name + --output HEADLESS-1
"$UMBRIEL" settle
moved=$("$UMBRIEL" windows --json | jq -r --arg id "$left" '.[] | select(.id == $id) | .workspace')
state | jq -e --arg id "$moved" '
  any(.[]; .id == $id and .name == "10" and .occupied)
  and all(.[]; .name != "+" or (.occupied | not))' > /dev/null
# If the only larger number is owned elsewhere, the empty slot cannot become
# an unnumbered active workspace.
msg workspace-switch 64 --output HEADLESS-2
foot --title=dynamic-last-number sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  last=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "dynamic-last-number") | .id')
  [[ -n $last ]] && break
  sleep 0.05
done
[[ -n $last ]]
msg workspace-switch 63 --output HEADLESS-1
foot --title=dynamic-penultimate sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  penultimate=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "dynamic-penultimate") | .id')
  [[ -n $penultimate ]] && break
  sleep 0.05
done
[[ -n $penultimate ]]
msg workspace-next
"$UMBRIEL" settle
state | jq -e 'any(.[]; .focused and .name == "63") and all(.[]; .name != "+" or (.active | not))' > /dev/null
"$UMBRIEL" swipe-test begin 2000 > /dev/null
"$UMBRIEL" swipe-test update 2100 0 -240 > /dev/null
"$UMBRIEL" swipe-test end 2600 0 > /dev/null
"$UMBRIEL" settle
state | jq -e 'any(.[]; .focused and .name == "63") and all(.[]; .name != "+" or (.active | not))' > /dev/null
sentinel=$(state | jq -r '.[] | select(.output == "HEADLESS-1" and .name == "+") | .id')
if [[ -n $sentinel ]]; then
  msg window-move-to-workspace-silent --name + --output HEADLESS-1
  "$UMBRIEL" settle
  "$UMBRIEL" windows --json | jq -e --arg id "$penultimate" '
    any(.[]; .id == $id and .workspace != "" and .output == "HEADLESS-1")' > /dev/null
  state | jq -e --arg id "$sentinel" 'any(.[]; .id == $id and (.occupied | not))' > /dev/null
fi
echo "global dynamic navigation can create a workspace without claiming another output's number"
