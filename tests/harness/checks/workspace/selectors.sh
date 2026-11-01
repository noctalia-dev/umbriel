#!/usr/bin/env bash
# harness: outputs=2
# Workspace selectors preserve their kind: bare numbers are positions on the cursor-preferred output, while names
# resolve globally. `--name` reaches numeric-looking names, `--output` addresses one group, and bad
# targets report useful errors.
set -euo pipefail

readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"

accepts() {
  if ! out=$("$UMBRIEL" msg "$@" 2>&1); then
    echo "expected '$*' to be accepted, got: $out"
    return 1
  fi
}

# Usage: rejects_with <expected-error> <action> [args...]
rejects_with() {
  local expected=$1
  shift
  if out=$("$UMBRIEL" msg "$@" 2>&1); then
    echo "expected '$*' to be rejected, but it succeeded"
    return 1
  fi
  if [[ $out != *"$expected"* ]]; then
    echo "expected '$*' to mention '$expected', got: $out"
    return 1
  fi
}

write_config() {
  printf '%s\n\n%s\n' "$BASELINE" "$1" > "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

expect_focused() {
  local output=$1 name=$2 named=$3 snapshot
  snapshot=$("$UMBRIEL" workspaces --json)
  if ! jq -e --arg output "$output" --arg name "$name" --argjson named "$named" '
    [.[] | select(.focused)] as $focused
    | ($focused | length) == 1
      and $focused[0].output == $output
      and $focused[0].name == $name
      and $focused[0].named == $named
  ' <<< "$snapshot" > /dev/null; then
    echo "expected focused workspace $output/$name with named=$named, got: $snapshot"
    return 1
  fi
}

expect_workspace() {
  local output=$1 index=$2 name=$3 named=$4 snapshot
  snapshot=$("$UMBRIEL" workspaces --json)
  if ! jq -e --arg output "$output" --argjson index "$index" --arg name "$name" --argjson named "$named" '
    any(.[]; .output == $output and .index == $index and .name == $name and .named == $named)
  ' <<< "$snapshot" > /dev/null; then
    echo "expected workspace $output position $index to be $name with named=$named, got: $snapshot"
    return 1
  fi
}

expect_workspace_layout() {
  local output=$1 index=$2 layout=$3 snapshot
  snapshot=$("$UMBRIEL" workspaces --json)
  if ! jq -e --arg output "$output" --argjson index "$index" --arg layout "$layout" '
    any(.[]; .output == $output and .index == $index and .layout == $layout)
  ' <<< "$snapshot" > /dev/null; then
    echo "expected workspace $output position $index to use $layout, got: $snapshot"
    return 1
  fi
}

rejects_with "unknown output: NOPE" workspace-switch 1 --output NOPE
rejects_with "unknown workspace: nosuchname" workspace-switch nosuchname
rejects_with "unknown output: NOPE" window-move-to-workspace 1 --output NOPE
rejects_with "unknown output: NOPE" column-move-to-workspace 1 --output NOPE

# A bare numeric selector stays positional on the cursor-preferred output even
# when another output has a static workspace with the same displayed label.
write_config '
[output.HEADLESS-1]
workspaces = 9

[output.HEADLESS-2]
workspaces = "dynamic"

[[workspace]]
name = "vesktop"
output = "HEADLESS-2"

[[workspace]]
name = "chat"
output = "HEADLESS-2"

[[workspace]]
index = 2
output = "HEADLESS-1"
layout.mode = "dwindle"

[[workspace]]
index = 3
output = "HEADLESS-1"
layout.mode = "master"'
expect_workspace HEADLESS-1 3 3 false
accepts workspace-switch 2 --output HEADLESS-1
accepts "workspace-move-down"
expect_focused HEADLESS-1 3 false
expect_workspace HEADLESS-1 2 2 false
expect_workspace HEADLESS-1 3 3 false
expect_workspace_layout HEADLESS-1 2 dwindle
expect_workspace_layout HEADLESS-1 3 master
accepts workspace-switch vesktop --output HEADLESS-2
accepts workspace-switch 2
expect_focused HEADLESS-2 chat true
accepts workspace-switch 3
expect_focused HEADLESS-2 3 false

# Numeric labels from a static count or a dynamic anonymous workspace never
# participate in name lookup.
rejects_with "unknown workspace on output HEADLESS-2: 3" workspace-switch --name 3 --output HEADLESS-2
rejects_with "unknown workspace on output HEADLESS-1: 3" workspace-switch --name 3 --output HEADLESS-1

# An explicit static name array supplies exact numeric names. Bare numbers stay
# positional on the cursor-preferred dynamic output.
write_config '
[output.HEADLESS-1]
workspaces = ["3", "2"]

[output.HEADLESS-2]
workspaces = "dynamic"

[[workspace]]
name = "vesktop"
output = "HEADLESS-2"

[[workspace]]
name = "chat"
output = "HEADLESS-2"'
accepts workspace-switch vesktop --output HEADLESS-2
accepts workspace-switch 3
expect_focused HEADLESS-2 3 false
accepts workspace-switch --name 3
expect_focused HEADLESS-1 3 true

# A `--name` numeric selector stays global, while a qualified bare number remains a
# position. A unique ordinary name can select its owning output globally.
accepts workspace-switch --name 2
expect_focused HEADLESS-1 2 true
accepts workspace-switch 2 --output HEADLESS-2
expect_focused HEADLESS-2 chat true
accepts workspace-switch --name 3 --output HEADLESS-1
accepts workspace-switch vesktop
expect_focused HEADLESS-2 vesktop true

# A full workspace transfer preserves its name and ID in the destination's fixed inventory.
write_config '
[output.HEADLESS-1]
position = [0, 0]
workspaces = 9

[output.HEADLESS-2]
position = [1280, 0]
workspaces = "dynamic"

[[workspace]]
name = "vesktop"
output = "HEADLESS-2"

[[window_rule]]
match.title = "^fixed-inventory-source$"
default_output = "HEADLESS-2"
default_workspace = "vesktop"'
foot --title=fixed-inventory-source sh -c 'sleep 120' > /dev/null 2>&1 &
source_id=
for _ in $(seq 40); do
  source_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "fixed-inventory-source") | .id')
  [[ -n $source_id ]] && break
  sleep 0.1
done
if [[ -z $source_id ]]; then
  echo "fixed-inventory source window did not map"
  exit 1
fi
"$UMBRIEL" msg window-focus-warp "$source_id" > /dev/null
source_workspace=$("$UMBRIEL" windows --json | jq -r --arg id "$source_id" '.[] | select(.id == $id) | .workspace')
if [[ $source_workspace != HEADLESS-2:* ]]; then
  echo "fixed-inventory source did not map on HEADLESS-2: $source_workspace"
  exit 1
fi
accepts "workspace-move-to-output-left"
snapshot=$("$UMBRIEL" workspaces --json)
transferred_workspace=$("$UMBRIEL" windows --json | jq -r --arg id "$source_id" '.[] | select(.id == $id) | .workspace')
if ! jq -e --arg id "$source_workspace" --arg transferred "$transferred_workspace" '
  $id == $transferred
    and any(.[]; .id == $id and .output == "HEADLESS-1" and .name == "vesktop" and .named)
    and ([.[] | select(.output == "HEADLESS-1")] | length == 10)
    and all(.[]; .output != "HEADLESS-2" or .name != "vesktop")
' <<< "$snapshot" > /dev/null; then
  echo "workspace transfer lost the named source identity: $snapshot"
  exit 1
fi
accepts workspace-switch vesktop --output HEADLESS-1
expect_focused HEADLESS-1 vesktop true
rejects_with "workspace" workspace-switch vesktop --output HEADLESS-2

# Protocol create requests cannot extend a fixed inventory.
before_create=$("$UMBRIEL" workspaces --json | jq -c 'sort_by(.output, .index)')
"$WORKSPACE" --create HEADLESS-1 EXTERNAL
after_create=$("$UMBRIEL" workspaces --json | jq -c 'sort_by(.output, .index)')
if [[ $after_create != "$before_create" ]]; then
  echo "protocol create request changed a fixed inventory: before=$before_create after=$after_create"
  exit 1
fi

echo "typed selectors resolve positions locally and names globally"
