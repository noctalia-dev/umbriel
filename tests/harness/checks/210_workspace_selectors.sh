#!/usr/bin/env bash
# harness: outputs=2
# Workspace selectors resolve exact names globally before falling back to a position on the focused output. Duplicate
# names stay on the focused output, qualified selectors address one group, and bad targets report useful errors.
set -euo pipefail

readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/workspace-client}"
readonly BASELINE="$(< "$UMBRIEL_CONFIG")"

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

expect_output() {
  local title=$1 output=$2 workspace
  workspace=$(field_of "$title" workspace)
  if [[ $workspace != "$output":* ]]; then
    echo "expected '$title' on $output after resolving the selector, got '$workspace'"
    return 1
  fi
}

workspace_name_of() {
  local id
  id=$(field_of "$1" workspace)
  "$WORKSPACE" --all | awk -F'\t' -v id="$id" '$1 == id { print $2 }'
}

# Dynamic workspaces resolve on the preferred output, and qualification selects one output explicitly.
accepts "workspace-switch:1"
accepts "workspace-switch:1/HEADLESS-1"
accepts "window-move-to-workspace:1"
accepts "workspace-switch:99"

rejects_with "workspace-switch:1/NOPE" "unknown output: NOPE"
rejects_with "workspace-switch:nosuchname" "unknown workspace: nosuchname"
rejects_with "window-move-to-workspace:1/NOPE" "unknown output: NOPE"

# A unique numeric name wins globally. This supports split ranges such as 1 through 5 on one output and 6 through 10
# on another without making the keybind depend on the currently focused output.
write_config '
[output.HEADLESS-1]
workspaces = ["1", "2", "3", "4", "5"]

[output.HEADLESS-2]
workspaces = ["6", "7", "8", "9", "10"]'

accepts "workspace-switch:6/HEADLESS-2"
accepts "workspace-switch:1"
spawn_client unique-one
wait_for_windows 1
expect_output unique-one HEADLESS-1

accepts "workspace-switch:6"
spawn_client unique-six
wait_for_windows 2
expect_output unique-six HEADLESS-2

# When the exact name exists on both outputs, the preferred output disambiguates it.
write_config '
[output.HEADLESS-1]
workspaces = ["1", "2", "3", "4", "5"]

[output.HEADLESS-2]
workspaces = ["1", "2", "3", "4", "5"]'
accepts "workspace-switch:1/HEADLESS-2"
accepts "workspace-switch:1"
spawn_client duplicate-one
wait_for_windows 3
expect_output duplicate-one HEADLESS-2

# With no exact numeric name anywhere, a number remains a positional selector on the preferred static output.
write_config '
[output.HEADLESS-1]
workspaces = ["一", "二", "三"]

[output.HEADLESS-2]
workspaces = ["WEB", "CHAT", "VIDEO"]'
accepts "workspace-switch:一/HEADLESS-1"
accepts "workspace-switch:2"
spawn_client positional-two
wait_for_windows 4
expect_output positional-two HEADLESS-1
if [[ $(workspace_name_of positional-two) != "二" ]]; then
  echo "numeric fallback did not select static workspace position 2: $(workspace_name_of positional-two)"
  exit 1
fi

echo "selectors resolve by unique name, preferred duplicate, or focused position; unknown targets are reported"
