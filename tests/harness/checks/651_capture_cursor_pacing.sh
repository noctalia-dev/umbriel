#!/usr/bin/env bash
# harness: outputs=2
# A hardware-cursor plane transition must reach a capture consumer that asked for cursor metadata: a move, both sides of
# an output crossing, and two non-motion transitions. The headless backend has no DRM plane, so the transition is
# synthesized with the harness-only plane-cursor command, which notifies compositors exactly as production does; the
# transition detector, the consumer gate, and the damage path under test are the real ones.
set -euo pipefail

left_log=$UMBRIEL_RUNTIME_DIR/capture-left.log
right_log=$UMBRIEL_RUNTIME_DIR/capture-right.log
"$UMBRIEL_CAPTURE_CLIENT" --cursor --output HEADLESS-1 > "$left_log" 2>&1 &
"$UMBRIEL_CAPTURE_CLIENT" --cursor --output HEADLESS-2 > "$right_log" 2>&1 &

frames() { grep -c '^frame ' "$1" 2>/dev/null || true; }

wait_ready() {
  for _ in $(seq 40); do
    grep -q session-ready "$1" 2>/dev/null && return 0
    sleep 0.1
  done
  echo "capture never started: $1"
  return 1
}

wait_frames() {
  for _ in $(seq 60); do
    [[ $(frames "$1") -ge $2 ]] && return 0
    sleep 0.05
  done
  echo "timed out waiting for $2 frame(s) in $1, got $(frames "$1")"
  return 1
}

wait_ready "$left_log"
wait_ready "$right_log"

# No transition, no damage: an idle output delivers nothing.
"$UMBRIEL" settle
left=$(frames "$left_log")
right=$(frames "$right_log")
"$UMBRIEL" settle
[[ $(frames "$left_log") -eq $left && $(frames "$right_log") -eq $right ]] || { echo "capture advanced with no transition"; exit 1; }
echo "  ok   idle outputs deliver nothing"

# A move inside the captured output.
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 1" > /dev/null
wait_frames "$left_log" $((left + 1))

# A crossing: 2000 is off the left output (1280 wide), 120 is inside the right one.
"$UMBRIEL" plane-cursor "HEADLESS-1 2000 100 1 2" > /dev/null
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 1 2" > /dev/null
wait_frames "$left_log" $((left + 2))
wait_frames "$right_log" $((right + 1))

# Non-motion transitions on the same path: hide, then a stationary image swap.
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 0 2" > /dev/null
wait_frames "$right_log" $((right + 2))
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 1 3" > /dev/null
wait_frames "$right_log" $((right + 3))

echo "  ok   move, crossing (source and destination), hide, and image swap each delivered a frame"
