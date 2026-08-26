#!/usr/bin/env bash
# harness: outputs=2
# Windows stranded when every output goes away at once (as a suspend/resume cycle does) must return to their own output
# and workspace once the monitors come back.
set -euo pipefail

readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/workspace-client}"

BASELINE="$(< "$UMBRIEL_CONFIG")"$'\n[appearance]\nanimation_ms = 1'

write_config() {
  {
    printf '%s\n' "$BASELINE"
    if [[ -n $1 ]]; then
      printf '\n%s\n' "$1"
    fi
  } > "$UMBRIEL_CONFIG"
}

log_mark() { wc -l < "$UMBRIEL_LOG"; }

wait_for_log_since() {
  local mark=$1 pattern=$2
  for _ in $(seq 40); do
    if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "$pattern"; then
      return 0
    fi
    sleep 0.25
  done
  return 1
}

expect_log_since() {
  local mark=$1 pattern=$2 message=$3
  if wait_for_log_since "$mark" "$pattern"; then
    return 0
  fi
  echo "$message"
  tail -8 "$UMBRIEL_LOG" | sed 's/^/  | /'
  exit 1
}

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

wait_for_workspace() {
  local title=$1 expected=$2 workspace=
  for _ in $(seq 40); do
    workspace=$(workspace_of "$title")
    [[ $workspace == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on workspace '$expected', got '$workspace'"
  return 1
}

# A workspace id carries a serial that an output going away and coming back does not preserve. Names survive it:
# assert on those.
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
  echo "expected '$title' back on '$expected', got '$home'"
  return 1
}

wait_for_output() {
  local title=$1 output=$2 workspace=
  for _ in $(seq 40); do
    workspace=$(workspace_of "$title")
    [[ $workspace == "$output":* ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on an output '$output' workspace, got '$workspace'"
  return 1
}

move_to_workspace() {
  local title=$1 selector=$2
  "$UMBRIEL" msg "window-focus:$(field_of "$title" id)" > /dev/null
  "$UMBRIEL" msg "window-move-to-workspace:$selector" > /dev/null
  wait_for_output "$title" "${selector#*/}"
}

# The setup below leans on three dynamic-group rules: an emptied workspace is reaped and the rest renumber, the active
# workspace is never reaped, and a running slide suspends both.
write_config ''
"$UMBRIEL" msg config-reload > /dev/null

# Two windows per output, each on its own dynamic workspace. HEADLESS-2's pair is spawned in reverse of the order its
# workspaces are created in.
spawn_client second-ws2
wait_for_count 1
spawn_client second-ws1
wait_for_count 2
move_to_workspace second-ws1 1/HEADLESS-2
move_to_workspace second-ws2 2/HEADLESS-2
spawn_client first-ws1
wait_for_count 3
move_to_workspace first-ws1 1/HEADLESS-1
spawn_client first-ws2
wait_for_count 4
move_to_workspace first-ws2 2/HEADLESS-1

declare -A HOME=()
for window in first-ws1 first-ws2 second-ws1 second-ws2; do
  HOME[$window]=$(home_of "$window")
done
if [[ $(printf '%s\n' "${HOME[@]}" | sort -u | wc -l) != 4 ]]; then
  echo "windows did not settle on four separate workspaces: ${HOME[*]}"
  exit 1
fi

# Windows are visited most-recently-focused first; this pins the order they leave in and come back in.
for window in second-ws1 second-ws2 first-ws2 first-ws1; do
  "$UMBRIEL" msg "window-focus:$(field_of "$window" id)" > /dev/null
done
# Off first-ws1's workspace: the active workspace is never reaped, and this one has to be.
"$UMBRIEL" msg workspace-switch:3/HEADLESS-1 > /dev/null

# Every monitor goes away at once, the way a suspend takes them; the windows have nowhere to go.
mark=$(log_mark)
write_config '[output.HEADLESS-1]
enabled = false
[output.HEADLESS-2]
enabled = false'
"$UMBRIEL" msg config-reload > /dev/null
for output in HEADLESS-1 HEADLESS-2; do
  expect_log_since "$mark" "output '$output': disabled by config" "$output was not disabled on reload"
done
for window in first-ws1 first-ws2 second-ws1 second-ws2; do
  wait_for_workspace "$window" ''
done

# HEADLESS-1 comes back first and takes in the windows whose own output is still missing.
mark=$(log_mark)
write_config '[output.HEADLESS-2]
enabled = false'
"$UMBRIEL" msg config-reload > /dev/null
expect_log_since "$mark" "output 'HEADLESS-1': applied mode=" "HEADLESS-1 was not re-enabled on reload"
wait_for_home first-ws1 "${HOME[first-ws1]}"
wait_for_home first-ws2 "${HOME[first-ws2]}"
wait_for_output second-ws1 HEADLESS-1
wait_for_output second-ws2 HEADLESS-1

# A window that sat out on another output still knows where it belongs.
mark=$(log_mark)
write_config ''
"$UMBRIEL" msg config-reload > /dev/null
expect_log_since "$mark" "output 'HEADLESS-2': applied mode=" "HEADLESS-2 was not re-enabled on reload"
for window in first-ws1 first-ws2 second-ws1 second-ws2; do
  wait_for_home "$window" "${HOME[$window]}"
done

echo "windows stranded when every output went away came back to their own output and workspace"
