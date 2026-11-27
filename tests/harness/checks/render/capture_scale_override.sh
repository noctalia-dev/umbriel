#!/usr/bin/env bash
# capture-scale resizes one window's image-copy capture source: a smaller
# override shrinks the buffers a capture session is offered, and 0/auto
# restores the output's own scale, live and for sessions started afterwards.
set -euo pipefail

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

rejects_with() {
  local expected=$1
  shift
  local out
  if out=$("$UMBRIEL" "$@" 2>&1); then
    echo "expected '$*' to be rejected, got: $out"
    exit 1
  fi
  if [[ $out != *"$expected"* ]]; then
    echo "expected '$*' to mention '$expected', got: $out"
    exit 1
  fi
}

"$UMBRIEL_UNMAP_CLIENT" cs-window 321 203 > "$UMBRIEL_RUNTIME_DIR/cs-client.log" 2>&1 &
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq 'length') == 1 ]] && break
  sleep 0.05
done
"$UMBRIEL" settle > /dev/null
window=$("$UMBRIEL" windows --json | jq -ce '.[0]')
window_id=$(jq -r '.id' <<< "$window")
width=$(jq -r '.w' <<< "$window")
height=$(jq -r '.h' <<< "$window")

# Argument validation runs before any window lookup matters.
rejects_with "usage: capture-scale" capture-scale "$window_id"
rejects_with "unknown window" capture-scale not-a-window auto
rejects_with "invalid scale" capture-scale "$window_id" banana
rejects_with "invalid scale" capture-scale "$window_id" 0.5junk
rejects_with "in [0, 1]" capture-scale "$window_id" -0.5
rejects_with "in [0, 1]" capture-scale "$window_id" 1.5
rejects_with "in [0, 1]" capture-scale "$window_id" nan

half_w=$(( (width + 1) / 2 ))
half_h=$(( (height + 1) / 2 ))

# The override applies to sessions that do not exist yet: set it before the
# capture client connects and its first constraints already come in halved.
"$UMBRIEL" capture-scale "$window_id" 0.5 > /dev/null
CAPTURE_LOG="$UMBRIEL_RUNTIME_DIR/cs-capture.log"
"$UMBRIEL_CAPTURE_CLIENT" --window "$window_id" > "$CAPTURE_LOG" 2>&1 &
capture_pid=$!
trap 'kill "$capture_pid" 2>/dev/null || true' EXIT

expect_capture() {
  local want_w=$1 want_h=$2 from_line=$3 description=$4
  for _ in $(seq 100); do
    if tail -n +"$from_line" "$CAPTURE_LOG" | awk -v label="window:$window_id" -v w="$want_w" -v h="$want_h" '
      $1 == "constraints" && $2 == label && $3 == w && $4 == h { constraints = 1 }
      $1 == "frame" && $2 == label && $7 == w && $8 == h { frame = 1 }
      END { exit !(constraints && frame) }
    '; then
      return 0
    fi
    sleep 0.05
  done
  echo "$description: no new constraints and delivered frame at ${want_w}x${want_h}"
  cat "$CAPTURE_LOG"
  exit 1
}

expect_capture "$half_w" "$half_h" 1 'initial capture under the 0.5 override'

from_line=$(($(wc -l < "$CAPTURE_LOG") + 1))
"$UMBRIEL" capture-scale "$window_id" auto > /dev/null
expect_capture "$width" "$height" "$from_line" 'auto restoring the output scale'

from_line=$(($(wc -l < "$CAPTURE_LOG") + 1))
"$UMBRIEL" capture-scale "$window_id" 0.5 > /dev/null
expect_capture "$half_w" "$half_h" "$from_line" 're-applying the override to a live session'

from_line=$(($(wc -l < "$CAPTURE_LOG") + 1))
"$UMBRIEL" capture-scale "$window_id" 0 > /dev/null
expect_capture "$width" "$height" "$from_line" 'zero scale restoring the output scale'

kill "$capture_pid"
wait "$capture_pid" || true
trap - EXIT

echo 'capture-scale shrank a window capture live and for later sessions, and auto/0 restored the output scale'
