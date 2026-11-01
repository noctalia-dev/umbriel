#!/usr/bin/env bash
# harness: outputs=2
# Native output actions use logical desktop enablement, so displaced windows
# return home and capture clients stop seeing a disabled output.
set -euo pipefail

BASELINE=$(< "$UMBRIEL_CONFIG")

spawn_client() {
  foot --title=output-action-rehome sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_workspace() {
  local expected=$1 workspace= windows=
  for _ in $(seq 40); do
    windows=$("$UMBRIEL" windows --json)
    if [[ $(jq 'length' <<< "$windows") -ne 1 ]]; then
      sleep 0.1
      continue
    fi
    workspace=$(jq -r '.[0].workspace // ""' <<< "$windows")
    [[ $workspace == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected window workspace '$expected', got '$workspace'"
  return 1
}

wait_for_temporary_workspace() {
  local native=$1 workspace=
  for _ in $(seq 40); do
    workspace=$("$UMBRIEL" windows --json | jq -r '.[0].workspace // ""')
    if [[ $workspace == HEADLESS-1:* && $workspace != "$native" ]] \
      && "$UMBRIEL" workspaces --json | jq -e --arg id "$workspace" \
        'any(.[]; .id == $id and .named == false)' > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected a separate anonymous temporary workspace on HEADLESS-1, got '$workspace' (native '$native')"
  return 1
}

wait_for_output_count() {
  local expected=$1 count=
  for _ in $(seq 40); do
    count=$("$UMBRIEL" outputs --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected output(s), got $count"
  return 1
}

wait_for_enabled() {
  local output=$1 expected=$2 state=
  for _ in $(seq 40); do
    state=$("$UMBRIEL" outputs --json | jq -r --arg output "$output" '.[] | select(.name == $output) | .enabled')
    [[ $state == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $output enabled state '$expected', got '$state'"
  return 1
}

expect_capture_failure() {
  local output=$1 status=
  if timeout 2s grim -o "$output" "$XDG_RUNTIME_DIR/output-action-disabled.png" > /dev/null 2>&1; then
    echo "expected capture of disabled output '$output' to fail"
    return 1
  else
    status=$?
  fi
  if [[ $status -eq 124 ]]; then
    echo "capture of disabled output '$output' timed out"
    return 1
  fi
}

spawn_client
wait_for_workspace 'HEADLESS-2:1'
native_refuge=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .index == 1) | .id')

"$UMBRIEL" msg output-disable HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 false
wait_for_temporary_workspace "$native_refuge"
expect_capture_failure HEADLESS-2
timeout 5s grim "$XDG_RUNTIME_DIR/output-action-desktop.png"

"$UMBRIEL" msg output-enable HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 true
wait_for_workspace 'HEADLESS-2:1'

"$UMBRIEL" msg output-toggle HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 false
wait_for_temporary_workspace "$native_refuge"
"$UMBRIEL" msg output-toggle HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 true
wait_for_workspace 'HEADLESS-2:1'

# A temporary logical override belongs to the compositor session, not to one
# backend Output object. Recreating the disabled head must not revive it or
# restore its displaced window onto an unavailable desktop target.
"$UMBRIEL" msg output-disable HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 false
wait_for_temporary_workspace "$native_refuge"
displaced_workspace=$("$UMBRIEL" windows --json | jq -r '.[0].workspace')
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
wait_for_output_count 1
"$UMBRIEL" output-create HEADLESS-2 > /dev/null
wait_for_output_count 2
wait_for_enabled HEADLESS-2 false
wait_for_workspace "$displaced_workspace"
expect_capture_failure HEADLESS-2

# Enabling a known disabled head must also work when it is the only output
# object and there is no live desktop fallback.
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
wait_for_output_count 1
wait_for_workspace ''
"$UMBRIEL" msg output-enable HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 true
wait_for_workspace 'HEADLESS-2:1'

# A config policy change supersedes temporary output-management state. The
# opposite action can then override that policy for the rest of the session,
# including destruction of every Output object and recreation of the monitor.
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
wait_for_output_count 0
wait_for_workspace ''
{
  printf '%s\n' "$BASELINE"
  printf '\n[output.HEADLESS-2]\nenabled = false\n'
} > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" output-create HEADLESS-2 > /dev/null
wait_for_output_count 1
wait_for_enabled HEADLESS-2 false
wait_for_workspace ''
"$UMBRIEL" msg output-enable HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 true
wait_for_workspace 'HEADLESS-2:1'
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
wait_for_output_count 0
wait_for_workspace ''
"$UMBRIEL" output-create HEADLESS-2 > /dev/null
wait_for_output_count 1
wait_for_enabled HEADLESS-2 true
wait_for_workspace 'HEADLESS-2:1'

if "$UMBRIEL" msg output-disable missing-output > /dev/null 2>&1; then
  echo "unknown output action unexpectedly succeeded"
  exit 1
fi

echo "native output actions override config, survive full output recreation, and restore displaced windows"
