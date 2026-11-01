#!/usr/bin/env bash
# harness: outputs=3
# Explicit destinations can pull inactive workspaces without changing the source selection.
set -euo pipefail

msg() { "$UMBRIEL" msg "$@" > /dev/null; }
state() { "$UMBRIEL" workspaces --json; }
id_of() { state | jq -r --arg name "$1" --arg output "$2" '.[] | select(.name == $name and .output == $output) | .id'; }
expect() {
  local snapshot
  snapshot=$(state)
  jq -e "$@" <<< "$snapshot" > /dev/null || { echo "unexpected workspaces: $snapshot"; exit 1; }
}
unchanged() {
  local reason=$1 before windows result
  shift
  "$UMBRIEL" settle
  before=$(state)
  windows=$("$UMBRIEL" windows --json)
  if [[ $reason != accept ]]; then
    if result=$("$UMBRIEL" msg "$@" 2>&1); then echo "unexpectedly accepted $*"; exit 1; fi
    [[ $result == *"$reason"* ]] || { echo "$* failed for the wrong reason: $result"; exit 1; }
  else
    msg "$@"
  fi
  "$UMBRIEL" settle
  [[ $(state) == "$before" ]] || { echo "$* changed inventory or selection"; exit 1; }
  [[ $("$UMBRIEL" windows --json) == "$windows" ]] || { echo "$* changed windows"; exit 1; }
}
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[output.HEADLESS-1]
position = [0, 0]
workspaces = ["WEB", "KEEP", "POSITION", "3", "My web"]
[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["TARGET", "WEB"]
[output.HEADLESS-3]
position = [2560, 0]
workspaces = ["OTHER", "TAIL"]
[[workspace]]
name = "WEB"
output = "HEADLESS-1"
layout.mode = "stacking"
CONFIG
msg config-reload
msg workspace-switch WEB --output HEADLESS-1
work=$(id_of WEB HEADLESS-1)
for title in explicit-one explicit-two; do
  "$UMBRIEL_UNMAP_CLIENT" "$title" 300 200 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  found=
  for _ in $(seq 60); do
    found=$("$UMBRIEL" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | .id')
    [[ -n $found ]] && break
    sleep 0.05
  done
  [[ -n $found ]]
done
msg workspace-switch KEEP --output HEADLESS-1
msg workspace-switch OTHER --output HEADLESS-3
unchanged 'ambiguous workspace' workspace-move-to-output current WEB
unchanged 'already has a workspace' workspace-move-to-output HEADLESS-2 WEB --output HEADLESS-1
unchanged 'unknown output' workspace-move-to-output NOPE WEB --output HEADLESS-1
unchanged 'unknown workspace' workspace-move-to-output current MISSING --output HEADLESS-1
unchanged 'unknown output' workspace-move-to-output current WEB --output NOPE

msg workspace-move-to-output current WEB --output HEADLESS-1
"$UMBRIEL" settle
expect --arg id "$work" '
  any(.[]; .id == $id and .output == "HEADLESS-3" and .index == 2 and .focused and .layout == "stacking")
  and any(.[]; .name == "KEEP" and .output == "HEADLESS-1" and .active)'
"$UMBRIEL" windows --json | jq -e --arg id "$work" 'length == 2 and all(.[]; .workspace == $id and .output == "HEADLESS-3")' > /dev/null
"$UMBRIEL_FOREIGN_TOPLEVEL_CLIENT" explicit-one HEADLESS-3

# An omitted source means the active workspace; connectors use ordinary output name matching.
msg workspace-move-to-output headless-1
expect --arg id "$work" 'any(.[]; .id == $id and .output == "HEADLESS-1" and .focused)'
unchanged accept workspace-move-to-output current
unchanged accept workspace-move-to-output HEADLESS-1 KEEP --output HEADLESS-1

# Unqualified duplicate names prefer the current output.
msg workspace-move-to-output HEADLESS-3 WEB
expect --arg id "$work" 'any(.[]; .id == $id and .output == "HEADLESS-3" and .focused)'
msg workspace-move-to-output HEADLESS-1

position=$(id_of POSITION HEADLESS-1)
named=$(id_of 3 HEADLESS-1)
spaced=$(id_of 'My web' HEADLESS-1)
msg workspace-switch TARGET --output HEADLESS-2
msg workspace-move-to-output current 3 --output HEADLESS-1
expect --arg id "$position" 'any(.[]; .id == $id and .output == "HEADLESS-2" and .focused)'
msg workspace-move-to-output current --name 3 --output HEADLESS-1
expect --arg id "$named" 'any(.[]; .id == $id and .output == "HEADLESS-2" and .focused)'
msg workspace-move-to-output --output HEADLESS-1 current 'My web'
expect --arg id "$spaced" 'any(.[]; .id == $id and .output == "HEADLESS-2" and .focused)'

msg output-disable HEADLESS-3
unchanged 'not available' workspace-move-to-output HEADLESS-3 WEB --output HEADLESS-1
msg output-enable HEADLESS-3
msg workspace-move-to-output HEADLESS-3 WEB --output HEADLESS-1
expect --arg id "$work" 'any(.[]; .id == $id and .output == "HEADLESS-3" and .focused)'

# A pinned window stays on its output when the hidden workspace it was pinned on moves away.
msg workspace-switch KEEP --output HEADLESS-1
"$UMBRIEL_UNMAP_CLIENT" explicit-pin 300 200 > "$UMBRIEL_RUNTIME_DIR/explicit-pin.log" 2>&1 &
pin=
for _ in $(seq 60); do
  pin=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "explicit-pin") | .id')
  [[ -n $pin ]] && break
  sleep 0.05
done
[[ -n $pin ]]
msg window-focus "$pin"
msg window-toggle-pinned
keep=$(id_of KEEP HEADLESS-1)
msg workspace-move-to-output current TARGET --output HEADLESS-2
target=$(id_of TARGET HEADLESS-1)
msg workspace-move-to-output HEADLESS-2 KEEP --output HEADLESS-1
"$UMBRIEL" settle
expect --arg id "$keep" 'any(.[]; .id == $id and .output == "HEADLESS-2" and .focused)'
"$UMBRIEL" windows --json | jq -e --arg id "$pin" --arg target "$target" '
  any(.[]; .id == $id and .pinned and .workspace == $target and .output == "HEADLESS-1")' > /dev/null
"$UMBRIEL_FOREIGN_TOPLEVEL_CLIENT" explicit-pin HEADLESS-1
echo "explicit targets preserve source selection, workspace identity and members; invalid targets are inert"
