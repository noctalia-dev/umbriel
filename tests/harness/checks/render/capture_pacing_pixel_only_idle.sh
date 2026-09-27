#!/usr/bin/env bash
# A capture consumer that did not ask for cursor metadata must not be woken by a plane-cursor transition, and an output
# with such a consumer keeps its damage-driven idle behavior.
set -euo pipefail

log=$UMBRIEL_RUNTIME_DIR/capture-pixel.log
"$UMBRIEL_CAPTURE_CLIENT" > "$log" 2>&1 &

source "$UMBRIEL_HARNESS_LIB"
await_lines "$log" session-ready 1 || { echo "pixel-only capture never started: $log"; exit 1; }

"$UMBRIEL" settle
before=$(events "$log" 'frame ')
for spec in "HEADLESS-1 100 100 1 1" "HEADLESS-1 2000 100 1 2" "HEADLESS-1 100 100 0 3"; do
  "$UMBRIEL" plane-cursor "$spec" > /dev/null
  "$UMBRIEL" settle
done
after=$(events "$log" 'frame ')
[[ $after -eq $before ]] || { echo "pixel-only capture was woken by cursor transitions: $after frame(s)"; exit 1; }
echo "  ok   pixel-only capture stayed idle across move/leave/hide"
