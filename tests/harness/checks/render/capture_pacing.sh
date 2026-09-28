#!/usr/bin/env bash
# harness: outputs=3
# A hardware-cursor plane transition must reach a capture consumer that asked for cursor metadata: a move, both sides of
# an output crossing, and non-motion transitions. A pixel-only consumer on a third output — where the pacing gate stays
# closed — must never be woken. The headless backend has no DRM plane, so transitions are synthesized with the
# harness-only plane-cursor command, which drives the production handleFrame path through the same needs_frame
# notification production gets; the transition detector, the consumer gate, and the damage path under test are the real
# ones.
set -euo pipefail

left_log=$UMBRIEL_RUNTIME_DIR/capture-left.log
right_log=$UMBRIEL_RUNTIME_DIR/capture-right.log
pixel_log=$UMBRIEL_RUNTIME_DIR/capture-pixel.log
"$UMBRIEL_CAPTURE_CLIENT" --cursor --output HEADLESS-1 > "$left_log" 2>&1 &
"$UMBRIEL_CAPTURE_CLIENT" --cursor --output HEADLESS-2 > "$right_log" 2>&1 &
"$UMBRIEL_CAPTURE_CLIENT" --output HEADLESS-3 > "$pixel_log" 2>&1 &

source "$UMBRIEL_HARNESS_LIB"
await_lines "$left_log" session-ready 1
await_lines "$right_log" session-ready 1
await_lines "$pixel_log" session-ready 1

# No transition, no damage: idle outputs deliver nothing.
"$UMBRIEL" settle
left=$(events "$left_log" 'frame ')
right=$(events "$right_log" 'frame ')
pixel=$(events "$pixel_log" 'frame ')
"$UMBRIEL" settle
[[ $(events "$left_log" 'frame ') -eq $left && $(events "$right_log" 'frame ') -eq $right && $(events "$pixel_log" 'frame ') -eq $pixel ]] || { echo "capture advanced with no transition"; exit 1; }
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

# Stationary geometry and hotspot changes take the same path: the box moves
# relative to the cursor position, so the diff and the damage are identical.
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 1 3 24 24 8 8" > /dev/null
await_lines "$right_log" 'frame ' $((right + 4)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 1 3 48 32 0 0" > /dev/null
await_lines "$right_log" 'frame ' $((right + 5)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 1 3 24 24 0 0" > /dev/null
await_lines "$right_log" 'frame ' $((right + 6)) 0.05

# The pixel-only consumer never asked for cursor metadata, so the gate stays
# closed on its output: none of the transitions above may have woken it.
after=$(events "$pixel_log" 'frame ')
[[ $after -eq $pixel ]] || { echo "pixel-only capture was woken by cursor transitions: $after frame(s)"; exit 1; }
echo "  ok   pixel-only capture stayed idle across move, crossing, hide, image, hotspot, and size"

# And back the other way: the destination of a crossing becomes its source.
"$UMBRIEL" plane-cursor "HEADLESS-2 2000 100 1 4" > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 4" > /dev/null
await_lines "$right_log" 'frame ' $((right + 7)) 0.05
await_lines "$left_log" 'frame ' $((left + 3)) 0.05

echo "  ok   move, crossings both directions, hide, image, hotspot, and size each delivered a frame"
