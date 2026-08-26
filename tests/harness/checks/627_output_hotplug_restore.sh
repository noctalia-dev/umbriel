#!/usr/bin/env bash
# harness: outputs=2
# The monitors a suspend takes away are destroyed outright, not disabled, and they come back as new outputs carrying
# the same names. Windows must find their way home across that too.
set -euo pipefail

readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/workspace-client}"

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_count() {
  local expected=$1 count=
  for _ in $(seq 40); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.25
  done
  echo "expected $expected window(s), got $count"
  return 1
}

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

workspace_of() { field_of "$1" workspace; }

home_of() {
  local workspace=
  workspace=$(workspace_of "$1")
  [[ -z $workspace ]] && return 0
  printf '%s/%s' "${workspace%%:*}" "$("$WORKSPACE" --all | awk -F'\t' -v id="$workspace" '$1 == id { print $2 }')"
}

wait_for_home() {
  local title=$1 expected=$2 home=
  for _ in $(seq 40); do
    home=$(home_of "$title")
    [[ $home == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on '$expected', got '$home'"
  return 1
}

move_to_workspace() {
  local title=$1 selector=$2
  "$UMBRIEL" msg "window-focus:$(field_of "$title" id)" > /dev/null
  "$UMBRIEL" msg "window-move-to-workspace:$selector" > /dev/null
  wait_for_home "$title" "${selector#*/}/${selector%%/*}"
}

wait_for_output() {
  local title=$1 output=$2 home=
  for _ in $(seq 40); do
    home=$(home_of "$title")
    [[ $home == "$output"/* ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on an output '$output' workspace, got '$home'"
  return 1
}

# Two windows on one output and one on the other, the same shape 625 uses, on dynamic workspaces.
spawn_client hotplug-first
wait_for_count 1
move_to_workspace hotplug-first 1/HEADLESS-1
spawn_client hotplug-second
wait_for_count 2
move_to_workspace hotplug-second 2/HEADLESS-1
spawn_client hotplug-other
wait_for_count 3
move_to_workspace hotplug-other 1/HEADLESS-2

# Unplug everything. Destroying the last output leaves the windows with no workspace at all.
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
for window in hotplug-first hotplug-second hotplug-other; do
  wait_for_home "$window" ''
done

# Plug the monitors back in one at a time, under the names they had before.
created=$("$UMBRIEL" output-create HEADLESS-2)
if [[ $created != HEADLESS-2 ]]; then
  echo "expected the new output to be named HEADLESS-2, got '$created'"
  exit 1
fi
wait_for_home hotplug-other HEADLESS-2/1
wait_for_output hotplug-first HEADLESS-2
wait_for_output hotplug-second HEADLESS-2

"$UMBRIEL" output-create HEADLESS-1 > /dev/null
wait_for_home hotplug-first HEADLESS-1/1
wait_for_home hotplug-second HEADLESS-1/2
wait_for_home hotplug-other HEADLESS-2/1

echo "windows came home across outputs being destroyed and recreated"
