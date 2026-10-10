#!/usr/bin/env bash
# A dialog parented through xdg-foreign, the way a portal attaches its file chooser to an application's window, opens
# centered over the window another process exported.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly PARENT_LOG="$UMBRIEL_RUNTIME_DIR/foreign-parent.log"
readonly CHILD_LOG="$UMBRIEL_RUNTIME_DIR/foreign-child.log"

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

[[window_rule]]
match.title = "^foreign-parent$"
default_floating = true
default_position = { x = 0, y = 0, anchor = "top_left" }
EOF2
"$UMBRIEL" msg config-reload > /dev/null

# The observer keeps its own 640x480, so the parent covers 0,0 to 640,480, and prints the handle it exported.
EXPORT_TOPLEVEL=1 "$OBSERVER" foreign-parent > "$PARENT_LOG" 2>&1 &
wait_for_window_count 1
handle=
for _ in $(seq 60); do
  handle=$(sed -n 's/^exported handle=//p' "$PARENT_LOG")
  [[ -n $handle ]] && break
  sleep 0.1
done
if [[ -z $handle ]]; then
  echo "the parent never received its xdg-foreign handle: $(cat "$PARENT_LOG")"
  exit 1
fi

# Another process imports the handle for its 400x300 dialog, which lands centered over the parent, at 120,90.
TRANSIENT_FOREIGN_HANDLE=$handle "$CLIENT" foreign-child 400 300 > "$CHILD_LOG" 2>&1 &
wait_for_window_count 2
wait_for_position foreign-child 120 90

echo "a dialog parented through xdg-foreign opens centered over the exported window"
