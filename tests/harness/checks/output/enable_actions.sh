#!/usr/bin/env bash
# harness: outputs=2
# Native output actions use logical desktop enablement, so displaced windows
# return home and capture clients stop seeing a disabled output.
set -euo pipefail

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
    workspace=$(jq -r '.[0].workspace' <<< "$windows")
    [[ $workspace == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected window workspace '$expected', got '$workspace'"
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

"$UMBRIEL" msg output-disable:HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 false
wait_for_workspace 'HEADLESS-1:1'
expect_capture_failure HEADLESS-2
timeout 5s grim "$XDG_RUNTIME_DIR/output-action-desktop.png"

"$UMBRIEL" msg output-enable:HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 true
wait_for_workspace 'HEADLESS-2:1'

"$UMBRIEL" msg output-toggle:HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 false
wait_for_workspace 'HEADLESS-1:1'
"$UMBRIEL" msg output-toggle:HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 true
wait_for_workspace 'HEADLESS-2:1'

if "$UMBRIEL" msg output-disable:missing-output > /dev/null 2>&1; then
  echo "unknown output action unexpectedly succeeded"
  exit 1
fi

echo "native output actions remove outputs from capture and restore displaced windows"
