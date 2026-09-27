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

source "$UMBRIEL_HARNESS_LIB"
await_lines "$left_log" session-ready 1
await_lines "$right_log" session-ready 1

# No transition, no damage: an idle output delivers nothing.
"$UMBRIEL" settle
left=$(events "$left_log" 'frame ')
right=$(events "$right_log" 'frame ')
"$UMBRIEL" settle
[[ $(events "$left_log" 'frame ') -eq $left && $(events "$right_log" 'frame ') -eq $right ]] || { echo "capture advanced with no transition"; exit 1; }
echo "  ok   idle outputs deliver nothing"

# A move inside the captured output.
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 1" > /dev/null
await_lines "$left_log" 'frame ' $((left + 1)) 0.05

# A crossing: 2000 is off the left output (1280 wide), 120 is inside the right one.
"$UMBRIEL" plane-cursor "HEADLESS-1 2000 100 1 2" > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 1 2" > /dev/null
await_lines "$left_log" 'frame ' $((left + 2)) 0.05
await_lines "$right_log" 'frame ' $((right + 1)) 0.05

# Non-motion transitions on the same path: hide, then a stationary image swap.
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 0 2" > /dev/null
await_lines "$right_log" 'frame ' $((right + 2)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 1 3" > /dev/null
await_lines "$right_log" 'frame ' $((right + 3)) 0.05

echo "  ok   move, crossing (source and destination), hide, and image swap each delivered a frame"
