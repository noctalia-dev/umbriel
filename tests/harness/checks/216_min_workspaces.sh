#!/usr/bin/env bash
# harness: outputs=2
# min_workspaces is a per-output floor on the dynamic workspace count. The configured output holds its floor while
# every workspace is empty, still grows a trailing empty above it, prunes back down to it, and leaves the other
# output's inventory alone. Lowering the floor on reload shrinks the inventory again.
set -euo pipefail

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

wait_for_windows() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  exit 1
}

workspace_index_of_window() {
  local workspace_id
  workspace_id=$("$UMBRIEL" windows --json | jq -r '.[0].workspace')
  "$UMBRIEL" workspaces --json | jq -r --arg id "$workspace_id" '.[] | select(.id == $id) | .index'
}

# A reload reaches each group through its own output projection, so the neighbour's inventory is the control here.
write_config '[output.HEADLESS-1]
min_workspaces = 3'
expect_names HEADLESS-1 "1 2 3" "floor applied on reload"
expect_names HEADLESS-2 "1" "floor scoped to its output"

# The floor is not a ceiling: occupying the last workspace still appends a trailing empty one.
"$UMBRIEL" msg workspace-switch:3/HEADLESS-1 > /dev/null
foot --title=min-workspaces-view sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_windows 1
expect_names HEADLESS-1 "1 2 3 4" "trailing empty above the floor"
if [[ $(workspace_index_of_window) != "3" ]]; then
  echo "view did not map on the third workspace: index=$(workspace_index_of_window)"
  exit 1
fi

window_id=$("$UMBRIEL" windows --json | jq -r '.[0].id')
"$UMBRIEL" msg "window-close:$window_id" > /dev/null
wait_for_windows 0
expect_names HEADLESS-1 "1 2 3" "pruning stops at the floor"

# Raising and lowering the floor without touching the output name set: the reload must reach this output's inventory
# through its own projection.
write_config '[output.HEADLESS-1]
min_workspaces = 5'
expect_names HEADLESS-1 "1 2 3 4 5" "raising the floor on reload"

write_config '[output.HEADLESS-1]
min_workspaces = 1'
expect_names HEADLESS-1 "1" "lowering the floor prunes on reload"

# Leaving a static inventory for a dynamic one fills up to the floor.
write_config '[output.HEADLESS-1]
workspaces = 2'
expect_names HEADLESS-1 "1 2" "static inventory"        # precondition for the fill below

write_config '[output.HEADLESS-1]
min_workspaces = 4'
expect_names HEADLESS-1 "1 2 3 4" "static inventory replaced by a floored dynamic one"

echo "min_workspaces holds a per-output floor, grows above it, and prunes back to it"
