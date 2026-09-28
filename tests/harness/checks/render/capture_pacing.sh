#!/usr/bin/env bash
# harness: outputs=2
# One check, one boot: plane_transition + source_only_crossing + pixel_only_idle folded together.
# HEADLESS-1 holds the cursor-metadata consumer (the captured side); HEADLESS-2 holds the pixel-only
# consumer, whose gate is closed — no plane transition or crossing on either output may wake it.
# The headless backend has no DRM plane, so transitions are synthesized with the harness-only
# plane-cursor command; the transition detector, the consumer gate, and the damage path are real.
set -euo pipefail

left_log=$UMBRIEL_RUNTIME_DIR/capture-left.log
pixel_log=$UMBRIEL_RUNTIME_DIR/capture-pixel.log
"$UMBRIEL_CAPTURE_CLIENT" --cursor --output HEADLESS-1 > "$left_log" 2>&1 &
"$UMBRIEL_CAPTURE_CLIENT" --output HEADLESS-2 > "$pixel_log" 2>&1 &

source "$UMBRIEL_HARNESS_LIB"
await_lines "$left_log" session-ready 1
await_lines "$pixel_log" session-ready 1

# No transition, no damage: idle outputs deliver nothing.
"$UMBRIEL" settle
left=$(events "$left_log" 'frame ')
pixel=$(events "$pixel_log" 'frame ')
"$UMBRIEL" settle
[[ $(events "$left_log" 'frame ') -eq $left && $(events "$pixel_log" 'frame ') -eq $pixel ]] || { echo "capture advanced with no transition"; exit 1; }
echo "  ok   idle outputs deliver nothing"

# plane_transition: a move inside the captured output.
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 1" > /dev/null
await_lines "$left_log" 'frame ' $((left + 1)) 0.05

# plane_transition: non-motion transitions on the captured side — hide, image, hotspot, size.
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 0 1" > /dev/null
await_lines "$left_log" 'frame ' $((left + 2)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 2" > /dev/null
await_lines "$left_log" 'frame ' $((left + 3)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 2 24 24 8 8" > /dev/null
await_lines "$left_log" 'frame ' $((left + 4)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 2 48 32 0 0" > /dev/null
await_lines "$left_log" 'frame ' $((left + 5)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 2 24 24 0 0" > /dev/null
await_lines "$left_log" 'frame ' $((left + 6)) 0.05

# Seed the pixel-only output's sample (invisible); its gate is closed, so this stays silent.
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 0 0" > /dev/null
"$UMBRIEL" settle

# source_only_crossing + pixel_only in one crossing: the leave must deliver on the captured
# source; landing on the pixel-only destination must not wake it. 2000 is off HEADLESS-1 (1280 wide).
"$UMBRIEL" plane-cursor "HEADLESS-1 2000 100 1 3" > /dev/null
await_lines "$left_log" 'frame ' $((left + 7)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 1 3" > /dev/null
"$UMBRIEL" settle
[[ $(events "$pixel_log" 'frame ') -eq $pixel ]] || { echo "pixel-only capture was woken landing on it"; exit 1; }

# pixel_only: plane transitions on the pixel-only output itself stay silent (move, hide, show).
for spec in "HEADLESS-2 300 100 1 4" "HEADLESS-2 300 100 0 4" "HEADLESS-2 300 100 1 4"; do
  "$UMBRIEL" plane-cursor "$spec" > /dev/null
  "$UMBRIEL" settle
done
[[ $(events "$pixel_log" 'frame ') -eq $pixel ]] || { echo "pixel-only capture was woken by transitions on its own output"; exit 1; }
echo "  ok   pixel-only capture stayed idle across the crossing and its own output's transitions"

# And back: the destination becomes its source — the leave on the uncaptured side is silent,
# the enter on the captured side delivers.
"$UMBRIEL" plane-cursor "HEADLESS-2 2000 100 0 5" > /dev/null
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 5" > /dev/null
await_lines "$left_log" 'frame ' $((left + 8)) 0.05
"$UMBRIEL" settle
[[ $(events "$pixel_log" 'frame ') -eq $pixel ]] || { echo "pixel-only capture was woken by the return crossing"; exit 1; }

echo "  ok   move, leave/enter crossing, non-motion transitions delivered; pixel-only never woken"
