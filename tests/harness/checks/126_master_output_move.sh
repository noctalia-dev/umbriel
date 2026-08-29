#!/usr/bin/env bash
# harness: outputs=2
# Master output moves transfer only the focused window, even for column-scoped actions.
set -euo pipefail

readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/workspace-client}"

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    return 1
  fi
}

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  local expected=$1 count=
  for _ in $(seq 40); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected window(s), got $count"
  return 1
}

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" \
    '.[] | select(.title == $title) | .[$field]'
}

wait_for_workspace() {
  local title=$1 expected=$2 actual=
  for _ in $(seq 50); do
    actual=$(field_of "$title" workspace)
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on $expected, got $actual"
  return 1
}

workspace_id_named() {
  "$WORKSPACE" --all | awk -F'\t' -v name="$1" '$2 == name { print $1; exit }'
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "master"

[animation]
duration_ms = 1

[output.HEADLESS-1]
position = [0, 0]
workspaces = ["ONE"]

[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["RIGHT_ONE"]
EOF
"$UMBRIEL" msg config-reload > /dev/null
one_id=$(workspace_id_named ONE)
right_one_id=$(workspace_id_named RIGHT_ONE)
if [[ -z $one_id || -z $right_one_id ]]; then
  echo "expected workspace ids for both outputs"
  exit 1
fi
accepts "workspace-switch:ONE/HEADLESS-1"

spawn_client master-a
wait_for_windows 1
spawn_client stack-b
wait_for_windows 2
spawn_client stack-c
wait_for_windows 3
stack_c_id=$(field_of stack-c id)

accepts column-move-to-output-right
wait_for_workspace master-a "$one_id"
wait_for_workspace stack-b "$one_id"
wait_for_workspace stack-c "$right_one_id"

accepts column-move-to-output-left
wait_for_workspace master-a "$one_id"
wait_for_workspace stack-b "$one_id"
wait_for_workspace stack-c "$one_id"

accepts "window-focus:$stack_c_id"
accepts window-move-or-output-right
wait_for_workspace master-a "$one_id"
wait_for_workspace stack-b "$one_id"
wait_for_workspace stack-c "$right_one_id"

echo "master column and edge output moves transferred only the focused window"
