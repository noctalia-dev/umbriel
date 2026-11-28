#!/usr/bin/env bash
# harness: outputs=2
# match.output selects opening rules against the output placement predicts. Rules whose output matches move the
# window in a circle have no fixed point; the window must still settle once instead of alternating resolutions.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

window_field() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" \
    '.[] | select(.title == $title) | .[$field]'
}

wait_for_window() {
  local title=$1 output=$2 entry=
  for _ in $(seq 80); do
    entry=$(window_field "$title" output)
    [[ $entry == "$output" && -n $(window_field "$title" w) ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on $output: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[output.HEADLESS-1]
position = [0, 0]
workspaces = ["LEFT"]

[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["RIGHT"]

[[window_rule]]
match.app_id = "^output-left$"
default_output = "HEADLESS-1"

[[window_rule]]
match.app_id = "^output-right$"
default_output = "HEADLESS-2"

[[window_rule]]
match.app_id = "^output-"
match.output = "HEADLESS-1"
default_scrolling_extent = 0.25

[[window_rule]]
match.app_id = "^output-"
match.output = "HEADLESS-2"
default_scrolling_extent = 0.75

[[window_rule]]
match.app_id = "^output-left$"
match.output = "HEADLESS-2"
title_bar = "forced"

[[window_rule]]
match.app_id = "^cycle$"
match.output = "HEADLESS-1"
default_output = "HEADLESS-2"

[[window_rule]]
match.app_id = "^cycle$"
match.output = "HEADLESS-2"
default_output = "HEADLESS-1"
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Keep the cursor-preferred output on HEADLESS-1 so the cycle has a known start.
read -r output_w output_h < <("$UMBRIEL" outputs --json |
  jq -r '.[] | select(.name == "HEADLESS-1") | .logical_size | "\(.width) \(.height)"')
"$POINTER" "$((output_w * 2))" "$output_h" move 10 10

APP_ID=output-left FOLLOW_CONFIGURES=1 "$CLIENT" output-left 800 600 \
  > "$UMBRIEL_RUNTIME_DIR/output-left.log" 2>&1 &
wait_for_window output-left HEADLESS-1

APP_ID=output-right FOLLOW_CONFIGURES=1 "$CLIENT" output-right 800 600 \
  > "$UMBRIEL_RUNTIME_DIR/output-right.log" 2>&1 &
wait_for_window output-right HEADLESS-2

APP_ID=cycle FOLLOW_CONFIGURES=1 "$CLIENT" cycle 800 600 \
  > "$UMBRIEL_RUNTIME_DIR/cycle.log" 2>&1 &
wait_for_window cycle HEADLESS-2

"$UMBRIEL" settle > /dev/null

left_width=$(window_field output-left w)
right_width=$(window_field output-right w)
cycle_output=$(window_field cycle output)
if ! [[ $left_width =~ ^[0-9]+$ && $right_width =~ ^[0-9]+$ ]] || ((right_width <= left_width)); then
  echo "match.output did not apply the destination output's extent: $("$UMBRIEL" windows --json)"
  exit 1
fi
if [[ $cycle_output != HEADLESS-2 ]]; then
  echo "cyclic output rules did not settle on the first destination: $("$UMBRIEL" windows --json)"
  exit 1
fi

if [[ $(window_field output-left title_bar) != false ]]; then
  echo "output-left showed the HEADLESS-2 title bar on HEADLESS-1"
  exit 1
fi

# Dynamic rules follow the window's output when its workspace moves, also for a window whose focus does not change.
APP_ID=output-left FOLLOW_CONFIGURES=1 "$CLIENT" output-left-focus 800 600 \
  > "$UMBRIEL_RUNTIME_DIR/output-left-focus.log" 2>&1 &
wait_for_window output-left-focus HEADLESS-1
"$UMBRIEL" msg workspace-move-to-output HEADLESS-2 LEFT > /dev/null
"$UMBRIEL" settle > /dev/null
if [[ $(window_field output-left output) != HEADLESS-2 || $(window_field output-left title_bar) != true ]]; then
  echo "moving output-left to HEADLESS-2 did not apply its output rule: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "match.output chose per-output extents, followed a moved workspace, and cyclic placement settled on HEADLESS-2"
