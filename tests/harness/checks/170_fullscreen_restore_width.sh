#!/usr/bin/env bash
# Entering fullscreen from a full-width column must preserve the column width that maximize saved for restoration.
set -euo pipefail

window_width() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "fullscreen-width") | .w'
}

readonly CLIENT="$(dirname "$UMBRIEL")/subsurface-client"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-width-client.log"

wait_for_width() {
  local expected=$1
  for _ in $(seq 60); do
    [[ $(window_width) == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "timed out waiting for width $expected: $($UMBRIEL windows --json)"
  return 1
}

"$CLIENT" fullscreen-width 640 480 > "$CLIENT_LOG" 2>&1 &

# The default 0.5 scrolling column is 624 px on the 1280x720 harness output.
readonly ORIGINAL_WIDTH=624
readonly FULL_WIDTH=1260
wait_for_width "$ORIGINAL_WIDTH"

"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_width "$FULL_WIDTH"

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_width 1280

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_width "$ORIGINAL_WIDTH"

echo "fullscreen round trip restored the maximized column from $FULL_WIDTH to $ORIGINAL_WIDTH"
