#!/usr/bin/env bash
# A dialog whose parent maps in the same dispatch opens centered on the parent's tile, which the layout has not
# arranged yet when the dialog is placed.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/transient-mapped-together.log"

windows() {
  "$UMBRIEL" windows --json
}

wait_for_window_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $(windows)"
  return 1
}

wait_for_position() {
  local title=$1 x=$2 y=$3
  for _ in $(seq 60); do
    if [[ $(windows | jq -r --arg title "$title" '.[] | select(.title == $title) | "\(.x) \(.y)"') == "$x $y" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $title at $x,$y, got: $(windows)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF2'

[animation]
enabled = false

[layout.scrolling]
default_extent_fraction = 0.5
center_underfull_strip = false
EOF2
"$UMBRIEL" msg config-reload > /dev/null

# The output is 1280x720 with an edge pad of 10, so the parent's column is 624 wide and 700 high at 10,10, and its
# 400x300 dialog lands at 122,210. Placed before the arrange, it would center on the output, at 440,210.
TRANSIENT_SUITE=mapped-together "$CLIENT" transient-child 400 300 > "$CLIENT_LOG" 2>&1 &
wait_for_window_count 2
wait_for_position transient-parent 10 10
wait_for_position transient-child 122 210

echo "a dialog mapped with its parent opens centered on the parent's tile"
