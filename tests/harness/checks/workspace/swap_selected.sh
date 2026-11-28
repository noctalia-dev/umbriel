#!/usr/bin/env bash
# harness: outputs=2
# workspace-swap exchanges the active workspace's windows with any selected
# workspace, on the same output or another, and follows them. Each dwindle split
# ratio arrives unchanged (every window box is translated only by the output
# offset), the workspaces keep their names and indices, and the selected
# workspace ends up active. Swapping with the active workspace is a no-op, and an
# unknown workspace is rejected.
set -euo pipefail

readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"
readonly OFFSET=1280

accepts() {
  if ! out=$("$UMBRIEL" msg "$@" 2>&1); then
    echo "expected '$*' to be accepted, got: $out"
    exit 1
  fi
}

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" \
    '.[] | select(.title == $title) | .[$field]'
}

wait_for_windows() {
  local expected=$1 count=
  for _ in $(seq 40); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    if [[ $count == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $expected window(s), got $count"
  exit 1
}

wait_for_workspace() {
  local title=$1 expected=$2 actual=
  for _ in $(seq 50); do
    actual=$(field_of "$title" workspace)
    if [[ $actual == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$title' on $expected, got $actual"
  exit 1
}

box_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" \
    '.[] | select(.title == $title) | "\(.workspace) \(.x) \(.y) \(.w) \(.h)"'
}

assert_box() {
  local title=$1 workspace=$2 dx=$3 expected=$4 actual=
  actual=$(box_of "$title")
  read -r _ x y w h <<< "$expected"
  local want="$workspace $((x + dx)) $y $w $h"
  if [[ $actual != "$want" ]]; then
    echo "$title: expected '$want', got '$actual'"
    exit 1
  fi
}

workspace_id_named() {
  "$WORKSPACE" --all | awk -F'\t' -v name="$1" '$2 == name { print $1; exit }'
}

active_workspace_name() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.focused) | .name'
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "dwindle"

[animation]
enabled = false

[input.cursor]
follows_focus = false

[output.HEADLESS-1]
position = [0, 0]
workspaces = ["ONE", "THREE"]

[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["TWO"]
EOF
"$UMBRIEL" msg config-reload > /dev/null

one=$(workspace_id_named ONE)
three=$(workspace_id_named THREE)
two=$(workspace_id_named TWO)
if [[ -z $one || -z $three || -z $two ]]; then
  echo "named workspaces missing: ONE='$one' THREE='$three' TWO='$two'"
  exit 1
fi

# Workspace ONE: three windows with a moved split, so only a replayed split tree
# lands on the same boxes. Workspace THREE holds a single window.
accepts workspace-switch ONE --output HEADLESS-1
spawn_client left
wait_for_windows 1
spawn_client top
wait_for_windows 2
spawn_client bottom
wait_for_windows 3
accepts window-focus "$(field_of left id)"
accepts window-modify-primary-extent -0.15
accepts workspace-switch THREE --output HEADLESS-1
spawn_client lone
wait_for_windows 4
accepts workspace-switch ONE --output HEADLESS-1
"$UMBRIEL" settle

left_before=$(box_of left)
top_before=$(box_of top)
bottom_before=$(box_of bottom)
lone_before=$(box_of lone)

# Same output: ONE <-> THREE, by name.
accepts workspace-swap THREE
wait_for_workspace left "$three"
"$UMBRIEL" settle
assert_box left "$three" 0 "$left_before"
assert_box top "$three" 0 "$top_before"
assert_box bottom "$three" 0 "$bottom_before"
assert_box lone "$one" 0 "$lone_before"
if [[ $(active_workspace_name) != THREE ]]; then
  echo "expected workspace THREE to be active after the swap, got $(active_workspace_name)"
  exit 1
fi
if [[ $(workspace_id_named ONE) != "$one" || $(workspace_id_named THREE) != "$three" ]]; then
  echo "workspace names moved with the windows"
  exit 1
fi

# Swapping with the active workspace changes nothing.
accepts workspace-swap THREE
"$UMBRIEL" settle
assert_box left "$three" 0 "$left_before"

# Another output: THREE <-> TWO.
accepts workspace-swap TWO --output HEADLESS-2
wait_for_workspace left "$two"
"$UMBRIEL" settle
assert_box left "$two" "$OFFSET" "$left_before"
assert_box top "$two" "$OFFSET" "$top_before"
assert_box bottom "$two" "$OFFSET" "$bottom_before"
if [[ $(active_workspace_name) != TWO ]]; then
  echo "expected workspace TWO to be active after the cross-output swap, got $(active_workspace_name)"
  exit 1
fi

if out=$("$UMBRIEL" msg workspace-swap nosuch 2>&1); then
  echo "expected an unknown workspace to be rejected, got: $out"
  exit 1
fi

echo "workspace-swap kept every split, kept names in place, and followed the windows"
