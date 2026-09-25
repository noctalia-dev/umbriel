#!/usr/bin/env bash
# harness: outputs=2
# A full wlr-output-management transaction may remove an output from the
# desktop and later restore it. Test requests must validate the same state
# without mutating layout, workspaces, or backend state.
set -euo pipefail

readonly OUTPUT_MANAGEMENT=${UMBRIEL_OUTPUT_MANAGEMENT_CLIENT:-./build-debug/tests/output-management-client}
readonly POINTER=${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}
readonly TEST_SHOT="$XDG_RUNTIME_DIR/output-management-test.png"

spawn_client() {
  foot --title=output-management-rehome sh -c 'sleep 120' > /dev/null 2>&1 &
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

wait_for_position() {
  local output=$1 expected_x=$2 expected_y=$3 position=
  for _ in $(seq 40); do
    position=$(
      "$UMBRIEL" outputs --json \
        | jq -r --arg output "$output" '.[] | select(.name == $output) | "\(.position.x),\(.position.y)"'
    )
    [[ $position == "$expected_x,$expected_y" ]] && return 0
    sleep 0.1
  done
  echo "expected $output position '$expected_x,$expected_y', got '$position'"
  return 1
}

expect_capture_failure() {
  local output=$1 shot=$2 status=
  if timeout 2s grim -o "$output" "$shot" > /dev/null 2>&1; then
    echo "expected capture of disabled output '$output' to fail"
    return 1
  else
    status=$?
  fi
  if [[ $status -eq 124 ]]; then
    echo "capture of disabled output '$output' timed out instead of rejecting the unavailable output"
    return 1
  fi
}

log_mark() { wc -l < "$UMBRIEL_LOG"; }

wait_for_log_since() {
  local mark=$1 pattern=$2
  for _ in $(seq 40); do
    if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "$pattern"; then
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for log: $pattern"
  return 1
}

spawn_client
wait_for_workspace 'HEADLESS-2:1'
read -r initial_x initial_y < <(
  "$UMBRIEL" outputs --json \
    | jq -r '.[] | select(.name == "HEADLESS-2") | "\(.position.x) \(.position.y)"'
)

# Testing the disable exercises backend validation without changing logical or
# physical state. A capture proves that the test did not commit backend state.
"$OUTPUT_MANAGEMENT" test disable HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 true
wait_for_workspace 'HEADLESS-2:1'
timeout 5s grim -o HEADLESS-2 "$TEST_SHOT"

# Applying the same request removes the output and displaces its window.
mark=$(log_mark)
"$OUTPUT_MANAGEMENT" apply disable HEADLESS-2 > /dev/null
wait_for_enabled HEADLESS-2 false
wait_for_workspace 'HEADLESS-1:1'
wait_for_log_since "$mark" "output 'HEADLESS-2': disabled by output management, power off"
expect_capture_failure HEADLESS-2 "$XDG_RUNTIME_DIR/output-management-disabled.png"

# Protocol disable is not DPMS. Input on the remaining output must not wake it.
"$POINTER" 1280 720 move 10 10
wait_for_enabled HEADLESS-2 false
wait_for_workspace 'HEADLESS-1:1'

# Re-enable at its original automatic-layout position. Its displaced window
# must return to the preserved workspace on that output.
"$OUTPUT_MANAGEMENT" apply enable HEADLESS-2 "$initial_x" "$initial_y" > /dev/null
wait_for_enabled HEADLESS-2 true
wait_for_position HEADLESS-2 "$initial_x" "$initial_y"
wait_for_workspace 'HEADLESS-2:1'

# If the only remaining logical output is asleep, disabling the powered output
# must preserve DPMS. Its displaced window waits until input wakes the logical
# fallback, then restores there instead of remaining unassigned.
"$UMBRIEL" msg window-move-to-workspace:1/HEADLESS-1 > /dev/null
wait_for_workspace 'HEADLESS-1:1'
mark=$(log_mark)
"$UMBRIEL" msg dpms-off:HEADLESS-2 > /dev/null
wait_for_log_since "$mark" "output 'HEADLESS-2': powered off"
mark=$(log_mark)
"$OUTPUT_MANAGEMENT" apply disable HEADLESS-1 > /dev/null
wait_for_enabled HEADLESS-1 false
wait_for_log_since "$mark" "output 'HEADLESS-2': enabled by output management, power off"
if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "output 'HEADLESS-2': applied mode="; then
  echo "output-management woke DPMS-off HEADLESS-2"
  exit 1
fi
mark=$(log_mark)
"$POINTER" 1280 720 move 20 20
wait_for_log_since "$mark" "output 'HEADLESS-2': applied mode="
wait_for_workspace 'HEADLESS-2:1'

echo "output-management test was inert, apply restored output state, and DPMS fallback recovery succeeded"
