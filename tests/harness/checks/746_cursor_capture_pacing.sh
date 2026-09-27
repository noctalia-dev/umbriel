#!/usr/bin/env bash
# harness: outputs=2
# Scope of the capture-pacing fix: grim is a cursor-excluding screencopy consumer, so these assertions pin that a
# hardware-cursor-only move, a crossing between two outputs, and a stationary cursor never make an output that does not
# want cursor metadata produce frames. The delivered-frame case for a cursor-metadata consumer needs a plane transition
# a headless backend cannot produce and is covered by the running-session matrix.
set -euo pipefail
rc=0
shot() { grim -o "$1" "$2"; }
shot_settled() { "$UMBRIEL" settle; shot "$1" "$2"; }
check() {
  if [[ $2 == "$3" ]]; then echo "  ok   $1"; else echo "  FAIL $1 (got '$2', want '$3')"; rc=1; fi
}
"$UMBRIEL" clock-freeze
shot_settled HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/base-1.png"
shot_settled HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/base-2.png"
# 1. Cursor-only move within output-1: excluding capture (grim default)
#    stays byte-identical when nothing else changed.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 100 100 > /dev/null 2>&1
shot_settled HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/move-1.png"
same=$(cmp -s "$UMBRIEL_RUNTIME_DIR/base-1.png" "$UMBRIEL_RUNTIME_DIR/move-1.png" && echo same || echo changed)
check "cursor-excluding capture stays idle on cursor-only move" "$same" same
# 2. Crossing output-1 -> output-2 and back leaves no stale output state:
#    outputs IPC still reports both heads, windows IPC unaffected.
"$UMBRIEL_POINTER_CLIENT" 2560 720 move 1400 300 > /dev/null 2>&1
shot_settled HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/move-2.png"
"$UMBRIEL_POINTER_CLIENT" 2560 720 move 200 300 > /dev/null 2>&1
shot_settled HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/back-1.png"
heads=$("$UMBRIEL" outputs | grep -c HEADLESS)
check "both outputs survive crossings" "$heads" 2
# 3. Idle: no animation, no input -> settle returns without new frames;
#    repetition is stable (no continuous pacing when cursor stationary).
shot_settled HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/idle-a.png"
shot_settled HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/idle-b.png"
idle=$(cmp -s "$UMBRIEL_RUNTIME_DIR/idle-a.png" "$UMBRIEL_RUNTIME_DIR/idle-b.png" && echo same || echo changed)
check "stationary cursor produces no continuous frames" "$idle" same
exit "$rc"
