#!/usr/bin/env bash
# harness: outputs=3
# A mirror follows the workspace it shows: when the original moves to another output the mirror keeps showing it, a move
# onto the mirror's own output stays refused, and when the original's output goes away the mirror follows the refuge its
# windows were evacuated to and returns to the original when the output comes back.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
msg() { "$UMBRIEL" msg "$@" > /dev/null; }

workspace_field() {
  "$UMBRIEL" workspaces --json |
    jq -r --arg out "$1" --arg name "$2" --arg field "$3" '.[] | select(.output == $out and .name == $name) | .[$field]'
}

expect_field() {
  local output=$1 name=$2 field=$3 want=$4 label=$5
  for _ in $(seq 60); do
    [[ $(workspace_field "$output" "$name" "$field") == "$want" ]] && return 0
    sleep 0.05
  done
  echo "$label: expected $output/$name $field='$want', got '$(workspace_field "$output" "$name" "$field")'"
  "$UMBRIEL" workspaces --json
  exit 1
}

printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[output.HEADLESS-1]
position = [0, 0]
workspaces = ["a", "b"]
[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["own", "b"]
[output.HEADLESS-3]
position = [2560, 0]
workspaces = ["far"]
[[workspace]]
output = "HEADLESS-2"
name = "b"
mirror = { output = "HEADLESS-1", name = "b" }
[[window_rule]]
match.title = "^follow-view$"
default_output = "HEADLESS-1"
default_workspace = "b"
CONFIG
msg config-reload

original_id=$(workspace_field HEADLESS-1 b id)
expect_field HEADLESS-2 b mirror_of "$original_id" "mirror shows the original"

# The original moves to another output: the mirror keeps showing that same workspace.
msg workspace-move-to-output HEADLESS-3 b --output HEADLESS-1
expect_field HEADLESS-2 b mirror_output HEADLESS-3 "mirror follows a moved original"
expect_field HEADLESS-2 b mirror_of "$original_id" "the followed workspace keeps its identity"

# Moving it onto the output that carries its mirror is refused, as any duplicate name is.
if "$UMBRIEL" msg workspace-move-to-output HEADLESS-2 b --output HEADLESS-3 > /dev/null 2>&1; then
  echo "the original was moved onto the output that carries its mirror"
  exit 1
fi
expect_field HEADLESS-2 b mirror_output HEADLESS-3 "refused move changes nothing"

msg workspace-move-to-output HEADLESS-1 b --output HEADLESS-3
expect_field HEADLESS-2 b mirror_output HEADLESS-1 "mirror follows the original home"

# Evacuation: the windows of b land on a refuge, and the mirror follows it.
foot --title=follow-view sh -c 'sleep 120' > /dev/null 2>&1 &
await_window follow-view
msg output-focus-right
msg output-focus-right
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
"$UMBRIEL" settle
refuge_id=$("$UMBRIEL" windows --json | jq -r '.[0].workspace')
expect_field HEADLESS-2 b mirror_of "$refuge_id" "mirror follows the refuge"
if [[ $refuge_id == "$original_id" ]]; then
  echo "the refuge is expected to be a new workspace"
  exit 1
fi

# The output returns, the windows go home, and the mirror shows the recreated original again.
"$UMBRIEL" output-create HEADLESS-1 > /dev/null
"$UMBRIEL" settle
home_id=$(workspace_field HEADLESS-1 b id)
expect_field HEADLESS-2 b mirror_of "$home_id" "mirror returns to the recreated original"
expect_field HEADLESS-2 b mirror_output HEADLESS-1 "mirror shows the original output again"

echo "a mirror follows its original across output moves and evacuation, and a duplicate move stays refused"
