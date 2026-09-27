#!/usr/bin/env bash
# A dialog that opens while its floating parent is still resizing to maximized centers over the size the parent is
# taking, not the one it has yet to leave, and stays there once the parent gets there.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly PARENT_LOG="$UMBRIEL_RUNTIME_DIR/maximizing-parent.log"
readonly CHILD_LOG="$UMBRIEL_RUNTIME_DIR/maximizing-child.log"
readonly PARENT_FIFO="$UMBRIEL_RUNTIME_DIR/maximizing-parent-control"

windows() {
  "$UMBRIEL" windows --json
}

field_of() {
  local title=$1 field=$2
  windows | jq -r --arg title "$title" --arg field "$field" '.[] | select(.title == $title) | .[$field]'
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

wait_for_size() {
  local title=$1 size=$2
  for _ in $(seq 60); do
    [[ "$(field_of "$title" w)x$(field_of "$title" h)" == "$size" ]] && return 0
    sleep 0.1
  done
  echo "expected $title at $size, got: $(windows)"
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
match.title = "^maximizing-parent$"
default_floating = true
default_position = { x = 0, y = 0, anchor = "top_left" }
EOF2
"$UMBRIEL" msg config-reload > /dev/null

# The parent keeps its own 640x480 at 0,0 and leaves the configure that maximizes it unanswered until told to, so it
# stays at that size with the maximize pending.
mkfifo "$PARENT_FIFO"
exec {parent_fd}<>"$PARENT_FIFO"
HOLD_RESIZE=1 EXPORT_TOPLEVEL=1 "$OBSERVER" maximizing-parent <&"$parent_fd" > "$PARENT_LOG" 2>&1 &
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
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null

# The 400x300 dialog centers over the whole 1280x720 output the parent is taking, at 440,210, not over its old box.
TRANSIENT_FOREIGN_HANDLE=$handle "$CLIENT" maximizing-child 400 300 > "$CHILD_LOG" 2>&1 &
wait_for_window_count 2
wait_for_position maximizing-child 440 210
if [[ "$(field_of maximizing-parent w)x$(field_of maximizing-parent h)" != 640x480 ]]; then
  echo "the parent was expected to still hold its 640x480: $(windows)"
  exit 1
fi

# Once the parent takes the output, the dialog is already centered over it.
printf r >&"$parent_fd"
wait_for_size maximizing-parent 1280x720
wait_for_position maximizing-child 440 210

echo "a dialog opened during its parent's maximize centers over the size the parent is taking"
