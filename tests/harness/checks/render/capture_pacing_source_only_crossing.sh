#!/usr/bin/env bash
# harness: outputs=2
# A crossing must leave the source output delivering frames even when the destination is not being captured. The
# reviewer's main correctness blocker: output A is already invisible by the time needs_frame fires, so the leave box has
# to come from the previous sample or the recorded cursor on A stays frozen until unrelated scene damage arrives.
set -euo pipefail

log=$UMBRIEL_RUNTIME_DIR/capture-source-only.log
"$UMBRIEL_CAPTURE_CLIENT" --cursor --output HEADLESS-1 > "$log" 2>&1 &

source "$UMBRIEL_HARNESS_LIB"
await_lines "$log" session-ready 1

"$UMBRIEL" settle
before=$(events "$log" 'frame ')

# Park the cursor inside the captured output, then move it off to the uncaptured one.
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 1" > /dev/null
await_lines "$log" 'frame ' $((before + 1)) 0.05
"$UMBRIEL" settle
crossed=$(events "$log" 'frame ')

# 2000 is off the left output (1280 wide) and inside neither captured session.
"$UMBRIEL" plane-cursor "HEADLESS-1 2000 100 1 2" > /dev/null
await_lines "$log" 'frame ' $((crossed + 1)) 0.05

echo "  ok   source-only capture kept delivering after the cursor left its output"
