#!/usr/bin/env bash
# Closing a focused Dwindle tile has two distinct stationary-pointer cases. A pointer over another existing survivor
# must not replace the layout predecessor chosen for a keyboard-focused close. A pointer over the closing tile must
# follow each survivor that expands into that position, including across consecutive closes without pointer motion.
# The clients stay alive after unmapping, so the assertions observe the unmap transition instead of relying on
# destroy-time fallback focus.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

pointer_pid=
cleanup() {
  if [[ -n $pointer_pid ]]; then
    kill "$pointer_pid" 2>/dev/null || true
    wait "$pointer_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  local title=$1
  "$CLIENT" "$title" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_focus() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$want" \
      '.[] | select(.id == $id) | (.focused and .active)') == true ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $want to hold workspace and keyboard focus: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_origin() {
  local id=$1 want_x=$2 want_y=$3
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$id" --argjson x "$want_x" --argjson y "$want_y" \
      '.[] | select(.id == $id) | (.x == $x and .y == $y)') == true ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $id to expand to ($want_x,$want_y): $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "dwindle"

[animation]
duration_ms = 1

[input.focus]
follows_mouse = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

# First preserve intentional keyboard focus. Park the pointer over the first leaf, focus the third leaf explicitly,
# and close it. The second leaf is its layout predecessor and must retain the resulting focus.
spawn_client "dwindle-close-pointer"
wait_for_count 1
spawn_client "dwindle-close-predecessor"
wait_for_count 2
spawn_client "dwindle-close-focused"
wait_for_count 3

# Let the one-millisecond opening animation settle before using target positions for hit-testing.
sleep 0.1
windows=$("$UMBRIEL" windows --json)
other_id=$(jq -r '.[] | select(.title == "dwindle-close-pointer") | .id' <<< "$windows")
predecessor_id=$(jq -r '.[] | select(.title == "dwindle-close-predecessor") | .id' <<< "$windows")
focused_id=$(jq -r '.[] | select(.title == "dwindle-close-focused") | .id' <<< "$windows")
other_x=$(jq -r '.[] | select(.title == "dwindle-close-pointer") | .x + 50' <<< "$windows")
other_y=$(jq -r '.[] | select(.title == "dwindle-close-pointer") | .y + 50' <<< "$windows")
focused_x=$(jq -r '.[] | select(.title == "dwindle-close-focused") | .x + 50' <<< "$windows")
focused_y=$(jq -r '.[] | select(.title == "dwindle-close-focused") | .y + 50' <<< "$windows")
if [[ -z $other_id || -z $predecessor_id || -z $focused_id ]]; then
  echo "could not resolve the predecessor scenario: $windows"
  exit 1
fi

# Cross from the focused tile into the first tile so follows_mouse observes a real enter. Restore keyboard focus to
# the newest window without moving the pointer, then close it.
pointer move "$focused_x" "$focused_y" move "$other_x" "$other_y"
wait_for_focus "$other_id"
"$UMBRIEL" msg "window-focus:$focused_id" > /dev/null
wait_for_focus "$focused_id"
"$UMBRIEL" msg "window-close:$focused_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/dwindle-close-focused.log" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/dwindle-close-focused.log"; then
  echo "keyboard-focused client did not unmap: $(< "$UMBRIEL_RUNTIME_DIR/dwindle-close-focused.log")"
  exit 1
fi
wait_for_count 2
wait_for_focus "$predecessor_id"

# Use another workspace for the close-under-pointer scenario so its Dwindle tree is independent of the first one.
"$UMBRIEL" msg workspace-switch:2 > /dev/null

# Repeated insertion builds a nested Dwindle branch. Closing the fourth leaf exposes the fifth, then closing the fifth
# exposes the sixth. The ordinary layout predecessor remains the third leaf, so both focus policies stay observably
# different across the consecutive closes.
count=2
for title in dwindle-close-a dwindle-close-b dwindle-reveal-predecessor dwindle-close-under-pointer \
  dwindle-close-expander dwindle-close-next-expander; do
  spawn_client "$title"
  count=$((count + 1))
  wait_for_count "$count"
done

# Let the opening animation settle before reading target positions for hit-testing.
sleep 0.1
windows=$("$UMBRIEL" windows --json)
closing_id=$(jq -r '.[] | select(.title == "dwindle-close-under-pointer") | .id' <<< "$windows")
expander_id=$(jq -r '.[] | select(.title == "dwindle-close-expander") | .id' <<< "$windows")
next_expander_id=$(jq -r '.[] | select(.title == "dwindle-close-next-expander") | .id' <<< "$windows")
closing_x=$(jq -r '.[] | select(.title == "dwindle-close-under-pointer") | .x' <<< "$windows")
closing_y=$(jq -r '.[] | select(.title == "dwindle-close-under-pointer") | .y' <<< "$windows")
if [[ -z $closing_id || -z $expander_id || -z $next_expander_id || -z $closing_x || -z $closing_y ]]; then
  echo "could not resolve Dwindle window state: $windows"
  exit 1
fi

# Keep one virtual pointer alive and motionless through the close. The offset is well inside the closing leaf, away
# from its gaps. Waiting for focus proves the initial move landed before the close request is sent.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$((closing_x + 50))" "$((closing_y + 50))" pause 10000 \
  > "$UMBRIEL_RUNTIME_DIR/stationary-pointer.log" 2>&1 &
pointer_pid=$!
wait_for_focus "$closing_id"

"$UMBRIEL" msg "window-close:$closing_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/dwindle-close-under-pointer.log" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/dwindle-close-under-pointer.log"; then
  echo "focused client did not unmap: $(< "$UMBRIEL_RUNTIME_DIR/dwindle-close-under-pointer.log")"
  exit 1
fi
wait_for_count 7

# First prove the intended survivor has taken over the cursor's old tile, then require focus to follow that scene
# transition without sending another pointer event.
wait_for_origin "$expander_id" "$closing_x" "$closing_y"
wait_for_focus "$expander_id"

# Close the revealed tile too, without moving the pointer. Its predecessor now expands into the same position and must
# inherit hover focus just like the first survivor did. This catches stale seat pointer focus left by the first unmap.
"$UMBRIEL" msg "window-close:$expander_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/dwindle-close-expander.log" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/dwindle-close-expander.log"; then
  echo "first revealed client did not unmap: $(< "$UMBRIEL_RUNTIME_DIR/dwindle-close-expander.log")"
  exit 1
fi
wait_for_count 6
wait_for_origin "$next_expander_id" "$closing_x" "$closing_y"
wait_for_focus "$next_expander_id"

echo "Dwindle close focus preserves keyboard intent across consecutive survivors revealed beneath a stationary pointer"
