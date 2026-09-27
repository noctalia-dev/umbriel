#!/usr/bin/env bash
# With hardware cursors locked the plane workaround must not run at all, even for a consumer that asked for cursor
# metadata: the painted-cursor path already produces its own damage.
set -euo pipefail

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[input.cursor]
hardware_cursor = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

log=$UMBRIEL_RUNTIME_DIR/capture-software.log
"$UMBRIEL_CAPTURE_CLIENT" --cursor > "$log" 2>&1 &

source "$UMBRIEL_HARNESS_LIB"
await_lines "$log" session-ready 1 || { echo "capture never started: $log"; exit 1; }

"$UMBRIEL" settle
before=$(events "$log" 'frame ')
"$UMBRIEL" plane-cursor "HEADLESS-1 100 100 1 1" > /dev/null
"$UMBRIEL" plane-cursor "HEADLESS-1 400 300 1 2" > /dev/null
"$UMBRIEL" settle
after=$(events "$log" 'frame ')
[[ $after -eq $before ]] || { echo "software-cursor output was paced: $after frame(s), was $before"; exit 1; }
echo "  ok   software-cursor output is never paced"
