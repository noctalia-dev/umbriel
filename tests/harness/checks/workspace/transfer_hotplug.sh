#!/usr/bin/env bash
# harness: outputs=3
# Explicit transfers adopt evacuated workspaces and keep IDs unique when their original output returns.
set -euo pipefail

readonly FOREIGN="${UMBRIEL_FOREIGN_TOPLEVEL_CLIENT:-./build-debug/tests/foreign-toplevel-client}"
msg() { "$UMBRIEL" msg "$@" > /dev/null; }
state() { "$UMBRIEL" workspaces --json; }
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[output.HEADLESS-1]
position = [0, 0]
workspaces = 2
[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["SECOND"]
[output.HEADLESS-3]
position = [2560, 0]
workspaces = ["THIRD"]
CONFIG
msg config-reload
msg workspace-switch 2 --output HEADLESS-1
foot --title=transfer-hotplug sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  window=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "transfer-hotplug") | .id')
  [[ -n $window ]] && break
  sleep 0.05
done
[[ -n $window ]]
original=$(state | jq -r '.[] | select(.output == "HEADLESS-1" and .index == 2) | .id')
msg workspace-move-to-output-right
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
"$UMBRIEL" output-create HEADLESS-1 > /dev/null
"$UMBRIEL" settle
state | jq -e --arg id "$original" '
  (map(.id) | length == (unique | length))
  and any(.[]; .id == $id and .output == "HEADLESS-2")' > /dev/null
"$FOREIGN" transfer-hotplug HEADLESS-2

# Hotplug evacuation records a home. A subsequent explicit transfer must adopt the workspace at its new output.
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
"$UMBRIEL" settle
refuge=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "transfer-hotplug") | .workspace')
evacuated=$(state | jq -r --arg id "$refuge" '.[] | select(.id == $id) | .output')
msg window-focus-warp "$window"
msg workspace-move-to-output-next
"$UMBRIEL" settle
adopted=$(state | jq -r --arg id "$refuge" '.[] | select(.id == $id) | .output')
[[ -n $adopted && $adopted != "$evacuated" ]]
"$UMBRIEL" output-create HEADLESS-2 > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" windows --json | jq -e --arg id "$refuge" --arg output "$adopted" '
  any(.[]; .title == "transfer-hotplug" and .workspace == $id and .output == $output)' > /dev/null
"$FOREIGN" transfer-hotplug "$adopted"
echo "transferred identities stay unique and explicit moves cancel hotplug return"
