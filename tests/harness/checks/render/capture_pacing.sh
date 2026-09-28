#!/usr/bin/env bash
# harness: outputs=2
# One check, one boot, one capture client: plane_transition + source_only_crossing + pixel_only_idle
# folded together. Both roles live in ONE process (one wl_client, the shape a real portal has):
# HEADLESS-1 holds the cursor-metadata session, HEADLESS-2 a pixel-only session — the same-client mixed
# case whose gate must key on the source output, since a client-level gate would wake HEADLESS-2 and
# fail here. The headless backend has no DRM plane, so transitions are synthesized with the harness-only
# plane-cursor command; the transition detector, the consumer gate, and the damage path are real.
set -euo pipefail

log=$UMBRIEL_RUNTIME_DIR/capture.log
"$UMBRIEL_CAPTURE_CLIENT" --cursor --output HEADLESS-1 --pixel-output HEADLESS-2 > "$log" 2>&1 &

source "$UMBRIEL_HARNESS_LIB"
await_lines "$log" 'session-ready HEADLESS-1' 1
await_lines "$log" 'session-ready HEADLESS-2' 1

# No transition, no damage: idle outputs deliver nothing.
"$UMBRIEL" settle
left=$(events "$log" 'frame HEADLESS-1')
pixel=$(events "$log" 'frame HEADLESS-2')
"$UMBRIEL" settle
[[ $(events "$log" 'frame HEADLESS-1') -eq $left && $(events "$log" 'frame HEADLESS-2') -eq $pixel ]] || { echo "capture advanced with no transition"; exit 1; }
echo "  ok   idle outputs deliver nothing"

# plane_transition: a move inside the captured output.
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 1" > /dev/null
await_lines "$log" 'frame HEADLESS-1' $((left + 1)) 0.05

# plane_transition: non-motion transitions on the captured side — hide, image, hotspot, size.
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 0 1" > /dev/null
await_lines "$log" 'frame HEADLESS-1' $((left + 2)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 2" > /dev/null
await_lines "$log" 'frame HEADLESS-1' $((left + 3)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 2 24 24 8 8" > /dev/null
await_lines "$log" 'frame HEADLESS-1' $((left + 4)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 2 48 32 0 0" > /dev/null
await_lines "$log" 'frame HEADLESS-1' $((left + 5)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 2 24 24 0 0" > /dev/null
await_lines "$log" 'frame HEADLESS-1' $((left + 6)) 0.05

# Seed the pixel-only output's sample (invisible); its gate is closed, so this stays silent.
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 0 0" > /dev/null
"$UMBRIEL" settle

# source_only_crossing + pixel_only in one crossing: the leave must deliver on the captured
# source; landing on the pixel-only destination must not wake it. 2000 is off HEADLESS-1 (1280 wide).
"$UMBRIEL" plane-cursor "HEADLESS-1 2000 100 1 3" > /dev/null
await_lines "$log" 'frame HEADLESS-1' $((left + 7)) 0.05
"$UMBRIEL" plane-cursor "HEADLESS-2 120 100 1 3" > /dev/null
"$UMBRIEL" settle
[[ $(events "$log" 'frame HEADLESS-2') -eq $pixel ]] || { echo "pixel-only capture was woken landing on it"; exit 1; }

# pixel_only: plane transitions on the pixel-only output itself stay silent (move, hide, show).
for spec in "HEADLESS-2 300 100 1 4" "HEADLESS-2 300 100 0 4" "HEADLESS-2 300 100 1 4"; do
  "$UMBRIEL" plane-cursor "$spec" > /dev/null
  "$UMBRIEL" settle
done
[[ $(events "$log" 'frame HEADLESS-2') -eq $pixel ]] || { echo "pixel-only capture was woken by transitions on its own output"; exit 1; }
echo "  ok   pixel-only capture stayed idle across the crossing and its own output's transitions"

# And back: the destination becomes its source — the leave on the uncaptured side is silent,
# the enter on the captured side delivers.
"$UMBRIEL" plane-cursor "HEADLESS-2 2000 100 0 5" > /dev/null
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 5" > /dev/null
await_lines "$log" 'frame HEADLESS-1' $((left + 8)) 0.05
"$UMBRIEL" settle
[[ $(events "$log" 'frame HEADLESS-2') -eq $pixel ]] || { echo "pixel-only capture was woken by the return crossing"; exit 1; }

echo "  ok   move, leave/enter crossing, non-motion transitions delivered; pixel-only never woken"
