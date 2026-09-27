#!/usr/bin/env bash
# harness: outputs=2
# Workspace selectors preserve their kind: bare numbers are positions on the cursor-preferred output, while names
# resolve globally. Quotes reach numeric-looking names, output qualifiers address one group, and bad
# targets report useful errors.
set -euo pipefail

readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    return 1
  fi
}

rejects_with() {
  local action=$1 expected=$2
  if out=$("$UMBRIEL" msg "$action" 2>&1); then
    echo "expected '$action' to be rejected, but it succeeded"
    return 1
  fi
  if [[ $out != *"$expected"* ]]; then
    echo "expected '$action' to mention '$expected', got: $out"
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

rejects_with "workspace-switch:1/NOPE" "unknown output: NOPE"
rejects_with "workspace-switch:nosuchname" "unknown workspace: nosuchname"
rejects_with "window-move-to-workspace:1/NOPE" "unknown output: NOPE"
rejects_with "column-move-to-workspace:1/NOPE" "unknown output: NOPE"

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
accepts "workspace-switch:2/HEADLESS-1"
accepts "workspace-move-down"
expect_focused HEADLESS-1 3 false
expect_workspace HEADLESS-1 2 2 false
expect_workspace HEADLESS-1 3 3 false
expect_workspace_layout HEADLESS-1 2 dwindle
expect_workspace_layout HEADLESS-1 3 master
accepts "workspace-switch:vesktop/HEADLESS-2"
accepts "workspace-switch:2"
expect_focused HEADLESS-2 chat true
accepts "workspace-switch:3"
expect_focused HEADLESS-2 3 false

# Numeric labels from a static count or a dynamic anonymous workspace never
# participate in name lookup.
rejects_with 'workspace-switch:"3"/HEADLESS-2' "unknown workspace on output HEADLESS-2: 3"
rejects_with 'workspace-switch:"3"/HEADLESS-1' "unknown workspace on output HEADLESS-1: 3"

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
accepts "workspace-switch:vesktop/HEADLESS-2"
accepts "workspace-switch:3"
expect_focused HEADLESS-2 3 false
accepts 'workspace-switch:"3"'
expect_focused HEADLESS-1 3 true

# A quoted numeric name stays global, while a qualified bare number remains a
# position. A unique ordinary name can select its owning output globally.
accepts 'workspace-switch:"2"'
expect_focused HEADLESS-1 2 true
accepts "workspace-switch:2/HEADLESS-2"
expect_focused HEADLESS-2 chat true
accepts 'workspace-switch:"3"/HEADLESS-1'
accepts "workspace-switch:vesktop"
expect_focused HEADLESS-2 vesktop true

# Moving a whole dynamic workspace reuses an empty fixed position instead of
# extending the fixed inventory.
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
"$UMBRIEL" msg "window-focus-warp:$source_id" > /dev/null
source_workspace=$("$UMBRIEL" windows --json | jq -r --arg id "$source_id" '.[] | select(.id == $id) | .workspace')
if [[ $source_workspace != HEADLESS-2:* ]]; then
  echo "fixed-inventory source did not map on HEADLESS-2: $source_workspace"
  exit 1
fi
fixed_nine_id=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .index == 9) | .id')
accepts "workspace-move-to-output-left"
snapshot=$("$UMBRIEL" workspaces --json)
if ! jq -e '
  [.[] | select(.output == "HEADLESS-1")] as $fixed
  | ($fixed | length) == 9 and all($fixed[]; .named == false)
' <<< "$snapshot" > /dev/null; then
  echo "workspace transfer changed the fixed inventory: $snapshot"
  exit 1
fi
transferred_workspace=$("$UMBRIEL" windows --json | jq -r --arg id "$source_id" '.[] | select(.id == $id) | .workspace')
if ! jq -e --arg id "$fixed_nine_id" --arg transferred "$transferred_workspace" '
  $id == $transferred
    and any(.[]; .id == $id and .output == "HEADLESS-1" and .index == 9 and .name == "9" and .named == false)
' <<< "$snapshot" > /dev/null; then
  echo "workspace transfer did not reuse fixed position 9: window=$transferred_workspace workspaces=$snapshot"
  exit 1
fi

# A full fixed inventory rejects the transfer without merging or moving its source.
write_config '
[output.HEADLESS-1]
position = [0, 0]
workspaces = 1

[output.HEADLESS-2]
position = [1280, 0]
workspaces = "dynamic"

[[workspace]]
name = "vesktop"
output = "HEADLESS-2"

[[window_rule]]
match.title = "^full-fixed-source$"
default_output = "HEADLESS-2"
default_workspace = "vesktop"'
foot --title=full-fixed-source sh -c 'sleep 120' > /dev/null 2>&1 &
full_source_id=
for _ in $(seq 40); do
  full_source_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "full-fixed-source") | .id')
  [[ -n $full_source_id ]] && break
  sleep 0.1
done
if [[ -z $full_source_id ]]; then
  echo "full fixed-inventory source window did not map"
  exit 1
fi
"$UMBRIEL" msg "window-focus-warp:$full_source_id" > /dev/null
full_source_workspace=$("$UMBRIEL" windows --json | jq -r --arg id "$full_source_id" '.[] | select(.id == $id) | .workspace')
rejects_with "workspace-move-to-output-left" "output has no empty fixed workspace"
if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$full_source_id" '.[] | select(.id == $id) | .workspace') != "$full_source_workspace" ]]; then
  echo "rejected full fixed-inventory transfer moved the source window"
  exit 1
fi
snapshot=$("$UMBRIEL" workspaces --json)
if ! jq -e '
  [.[] | select(.output == "HEADLESS-1")]
  | length == 1 and .[0].index == 1 and .[0].name == "1" and .[0].named == false
' <<< "$snapshot" > /dev/null; then
  echo "rejected transfer changed the full fixed inventory: $snapshot"
  exit 1
fi

# Explicitly named fixed slots are reusable without changing their identity.
write_config '
[output.HEADLESS-1]
position = [0, 0]
workspaces = ["ONE", "TWO"]

[output.HEADLESS-2]
position = [1280, 0]
workspaces = "dynamic"

[[workspace]]
name = "vesktop"
output = "HEADLESS-2"'
"$UMBRIEL" msg "window-focus-warp:$full_source_id" > /dev/null
fixed_two_id=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .name == "TWO") | .id')
accepts "workspace-move-to-output-left"
transferred_workspace=$("$UMBRIEL" windows --json | jq -r --arg id "$full_source_id" '.[] | select(.id == $id) | .workspace')
snapshot=$("$UMBRIEL" workspaces --json)
if ! jq -e --arg id "$fixed_two_id" --arg transferred "$transferred_workspace" '
  $id == $transferred
    and any(.[]; .id == $id and .output == "HEADLESS-1" and .index == 2 and .name == "TWO" and .named == true)
' <<< "$snapshot" > /dev/null; then
  echo "workspace transfer did not reuse named fixed slot TWO: window=$transferred_workspace workspaces=$snapshot"
  exit 1
fi

# Protocol create requests cannot extend a fixed inventory either.
before_create=$("$UMBRIEL" workspaces --json | jq -c 'sort_by(.output, .index)')
"$WORKSPACE" --create HEADLESS-1 EXTERNAL
after_create=$("$UMBRIEL" workspaces --json | jq -c 'sort_by(.output, .index)')
if [[ $after_create != "$before_create" ]]; then
  echo "protocol create request changed a fixed inventory: before=$before_create after=$after_create"
  exit 1
fi

echo "typed selectors resolve positions locally and names globally"
