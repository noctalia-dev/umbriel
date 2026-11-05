#!/usr/bin/env bash
# harness: outputs=2
# A mirror is a member of the receiving output's inventory: it sits at its declared position, carries the origin's name,
# and reports which workspace it shows. Windows launched from a mirror join its source, never the passive copy itself.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BASELINE="$(< "$UMBRIEL_CONFIG")"

write_config() {
  printf '%s\n%s\n' "$BASELINE" "$1" > "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

names_on() {
  "$UMBRIEL" workspaces --json |
    jq -r --arg out "$1" '[.[] | select(.output == $out)] | sort_by(.index) | map(.name) | join(" ")'
}

expect_names() {
  local output=$1 want=$2 label=$3
  for _ in $(seq 40); do
    [[ $(names_on "$output") == "$want" ]] && return 0
    sleep 0.05
  done
  echo "$label: expected '$want' on $output, got '$(names_on "$output")'"
  exit 1
}

workspace_field() {
  "$UMBRIEL" workspaces --json |
    jq -r --arg out "$1" --arg name "$2" --arg field "$3" '.[] | select(.output == $out and .name == $name) | .[$field]'
}

expect_field() {
  local output=$1 name=$2 field=$3 want=$4 label=$5
  for _ in $(seq 40); do
    [[ $(workspace_field "$output" "$name" "$field") == "$want" ]] && return 0
    sleep 0.05
  done
  echo "$label: expected $output/$name $field='$want', got '$(workspace_field "$output" "$name" "$field")'"
  exit 1
}

write_config '[output.HEADLESS-1]
position = [0, 0]
workspaces = ["one", "two", "three"]
[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["own", "two@HEADLESS-1", "three@HEADLESS-1"]'

expect_names HEADLESS-1 "one two three" "origin inventory"
expect_names HEADLESS-2 "own two three" "mirrors sit at their declared positions"

origin_id=$(workspace_field HEADLESS-1 two id)
expect_field HEADLESS-2 two mirror true "mirror flag"
expect_field HEADLESS-2 two mirror_of "$origin_id" "mirror points at the origin workspace"
expect_field HEADLESS-2 two mirror_output HEADLESS-1 "mirror names the origin output"
expect_field HEADLESS-1 two mirror false "origin is no mirror"
expect_field HEADLESS-1 two mirror_of null "origin shows nothing"

# The bar reads ext-workspace, not IPC: the copy must be advertised in the receiving output's protocol group too.
mirror_id=$(workspace_field HEADLESS-2 two id)
protocol_groups=$("$UMBRIEL_WORKSPACE_CLIENT" --groups)
expected_group=$(printf '%s\ttwo\tHEADLESS-2' "$mirror_id")
if ! printf '%s\n' "$protocol_groups" | grep -Fx -- "$expected_group" > /dev/null; then
  echo "mirror missing from the receiving ext-workspace group: $protocol_groups"
  exit 1
fi

# A mirror of another name takes its own place; reordering the list moves the mirror with it.
write_config '[output.HEADLESS-1]
position = [0, 0]
workspaces = ["one", "two", "three"]
[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["three@HEADLESS-1", "own", "two@HEADLESS-1"]
mode = "960x720"
[layout.scrolling]
default_extent_fraction = 0.5
[[window_rule]]
match.title = "^mirror-inventory-rule$"
default_output = "HEADLESS-2"
default_workspace = "own"'
expect_names HEADLESS-2 "three own two" "reordered mirrors"
expect_field HEADLESS-2 three mirror_of "$(workspace_field HEADLESS-1 three id)" "reordered mirror follows its name"

# A launch from the receiving output joins the copied source, without activating it on the origin output.
[[ $(workspace_field HEADLESS-2 own active) == true ]]
"$UMBRIEL" msg workspace-switch two --output HEADLESS-2 > /dev/null
expect_field HEADLESS-2 two active true "a mirror can be activated by name"
if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.focused).output') != HEADLESS-2 ]]; then
  "$UMBRIEL" msg output-focus-next > /dev/null
fi
[[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.focused).output') == HEADLESS-2 ]]
foot --title=mirror-inventory-view sh -c 'sleep 120' > /dev/null 2>&1 &
await_window mirror-inventory-view
view_workspace=$("$UMBRIEL" windows --json | jq -r '.[0].workspace')
if [[ $("$UMBRIEL" workspaces --json | jq -r --arg id "$view_workspace" '.[] | select(.id == $id) | .mirror') != false ]]; then
  echo "a window opened on a mirror workspace: $view_workspace"
  exit 1
fi
expected_workspace=$(workspace_field HEADLESS-1 two id)
if [[ $view_workspace != "$expected_workspace" ]]; then
  echo "a window launched from the mirror should open on HEADLESS-1/two: got $view_workspace"
  exit 1
fi
view_width=$("$UMBRIEL" windows --json | jq -r '.[0].w')
if ((view_width < 600 || view_width > 660)); then
  echo "the opening size should use the 1280px source, not its 960px copy: got $view_width"
  exit 1
fi
expect_field HEADLESS-1 one active true "launch keeps the original output on its chosen workspace"
expect_field HEADLESS-2 two active true "launch keeps the receiving output on the copy"

# Keybinds and the launcher use compositor-issued activation tokens: they must preserve the same placement and focus.
"$UMBRIEL" msg spawn "exec foot --title=mirror-inventory-spawn sh -c 'sleep 120'" > /dev/null
await_window mirror-inventory-spawn
"$UMBRIEL" settle
spawned=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "mirror-inventory-spawn")')
[[ $(jq -r '.workspace' <<<"$spawned") == "$expected_workspace" ]]
[[ $(jq -r '.focused' <<<"$spawned") == true ]]
expect_field HEADLESS-1 one active true "token-backed launch does not activate the source"
expect_field HEADLESS-2 two active true "token-backed launch retains the copy"

# Explicit placement wins over the mirrored launch origin.
"$UMBRIEL" msg spawn "exec foot --title=mirror-inventory-rule sh -c 'sleep 120'" > /dev/null
await_window mirror-inventory-rule
rule_workspace=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "mirror-inventory-rule") | .workspace')
[[ $rule_workspace == "$(workspace_field HEADLESS-2 own id)" ]]

# Launching from an ordinary workspace remains local.
"$UMBRIEL" msg workspace-switch own --output HEADLESS-2 > /dev/null
"$UMBRIEL" msg spawn "exec foot --title=mirror-inventory-own sh -c 'sleep 120'" > /dev/null
await_window mirror-inventory-own
own_workspace=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "mirror-inventory-own") | .workspace')
[[ $own_workspace == "$(workspace_field HEADLESS-2 own id)" ]]

# Removing the origin entry leaves a mirror with nothing to show, which reports no origin.
write_config '[output.HEADLESS-1]
position = [0, 0]
workspaces = ["one", "three"]
[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["own", "three@HEADLESS-1"]'
expect_names HEADLESS-2 "own three" "mirror of a removed entry is dropped from the list"

echo "mirrors take their declared place, report their origin, and never receive windows"
