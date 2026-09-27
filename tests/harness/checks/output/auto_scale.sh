#!/usr/bin/env bash
# harness: outputs=2
# An output without a position follows the scaled logical edge of the explicitly positioned output. Removing an
# explicit scale restores the default and moves the automatic output with that edge, while pointer crossing keeps
# working in both layouts.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
BASELINE=$(< "$UMBRIEL_CONFIG")

write_config() {
  {
    printf '%s\n' "$BASELINE"
    printf '\n[input.focus]\nfollows_mouse = true\n'
    printf '\n[output.HEADLESS-1]\nposition = [0, 0]\n'
    [[ -n $1 ]] && printf 'scale = %s\n' "$1"
    printf '\n[[window_rule]]\nmatch.title = "^auto-scale-left$"\ndefault_output = "HEADLESS-1"\n'
    printf '\n[[window_rule]]\nmatch.title = "^auto-scale-right$"\ndefault_output = "HEADLESS-2"\n'
  } > "$UMBRIEL_CONFIG"
}

output_x() {
  "$UMBRIEL" outputs \
    | awk -v name="$1" '$1 == name { found = 1; next } found && /Position:/ { split($2, p, ","); print p[1]; exit }'
}

output_scale() {
  "$UMBRIEL" outputs | awk -v name="$1" '$1 == name { found = 1; next } found && /Scale:/ { print $2; exit }'
}

wait_for_output() {
  local name=$1 expected_x=$2 expected_scale=$3 actual_x= actual_scale=
  for _ in $(seq 40); do
    actual_x=$(output_x "$name")
    actual_scale=$(output_scale "$name")
    [[ $actual_x == "$expected_x" && $actual_scale == "$expected_scale" ]] && return 0
    sleep 0.1
  done
  echo "$name state did not settle: x=$actual_x scale=$actual_scale, expected x=$expected_x scale=$expected_scale"
  "$UMBRIEL" outputs
  return 1
}

wait_for_windows() {
  local windows=
  for _ in $(seq 60); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e '
      any(.[]; .title == "auto-scale-left" and (.workspace | startswith("HEADLESS-1:")))
      and any(.[]; .title == "auto-scale-right" and (.workspace | startswith("HEADLESS-2:")))
    ' <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "clients did not map on their configured outputs: $windows"
  return 1
}

wait_for_focus() {
  local expected=$1 focused=
  for _ in $(seq 40); do
    focused=$("$UMBRIEL" windows --json | jq -r '.[] | select(.active) | .title')
    [[ $focused == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected focus on $expected, got $focused"
  return 1
}

write_config 1.25
"$UMBRIEL" msg config-reload > /dev/null
wait_for_output HEADLESS-1 0 1.250000
wait_for_output HEADLESS-2 1024 1.000000

foot --title=auto-scale-left sh -c 'sleep 120' > /dev/null 2>&1 &
foot --title=auto-scale-right sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_windows
"$POINTER" 2304 720 move 100 100 move 1100 100
wait_for_focus auto-scale-right

write_config ''
"$UMBRIEL" msg config-reload > /dev/null
wait_for_output HEADLESS-1 0 1.000000
wait_for_output HEADLESS-2 1280 1.000000
"$POINTER" 2560 720 move 100 100 move 1360 100
wait_for_focus auto-scale-right

echo "automatic output placement followed scale changes and the pointer crossed both shared edges"
