#!/usr/bin/env bash
# `focused-window` answers with the window the window-actions policy calls
# focused: the seat's focused view, else the active workspace's remembered
# focus. The reply is exactly a `windows` entry, and a layer-shell surface
# borrowing the keyboard must not change the answer.
set -euo pipefail

fail() {
  echo "$1"
  exit 1
}

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[input.focus]
follows_mouse = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

windows_json() {
  "$UMBRIEL" windows --json
}

# Nothing mapped: "no focused window" is an error, not an empty success.
if out=$("$UMBRIEL" focused-window 2> "$UMBRIEL_RUNTIME_DIR/focused-window-none.err"); then
  fail "focused-window answered rc 0 with no windows: $out"
fi
if ! grep -q "no focused window" "$UMBRIEL_RUNTIME_DIR/focused-window-none.err"; then
  fail "stderr lacks 'no focused window': $(cat "$UMBRIEL_RUNTIME_DIR/focused-window-none.err")"
fi

"$UMBRIEL_UNMAP_CLIENT" fw-a 600 400 > "$UMBRIEL_RUNTIME_DIR/focused-window-a.log" 2>&1 &
"$UMBRIEL_UNMAP_CLIENT" fw-b 600 400 > "$UMBRIEL_RUNTIME_DIR/focused-window-b.log" 2>&1 &
for _ in $(seq 100); do
  [[ $(windows_json | jq 'length') == 2 ]] && break
  sleep 0.05
done
[[ $(windows_json | jq 'length') == 2 ]] || fail "expected two windows: $(windows_json)"
"$UMBRIEL" settle > /dev/null

id_a=$(windows_json | jq -r '.[] | select(.title == "fw-a") | .id')
id_b=$(windows_json | jq -r '.[] | select(.title == "fw-b") | .id')
[[ -n $id_a && -n $id_b ]] || fail "missing fw-a/fw-b ids: $(windows_json)"

# Focus fw-a and wait for the activation to land.
"$UMBRIEL" msg "window-focus:$id_a" > /dev/null
for _ in $(seq 100); do
  windows_json | jq -e --arg id "$id_a" 'any(.[]; .active and .id == $id)' > /dev/null && break
  sleep 0.05
done
windows_json | jq -e --arg id "$id_a" 'any(.[]; .active and .id == $id)' > /dev/null \
  || fail "fw-a never became active: $(windows_json)"

# The JSON reply equals the windows entry for that window.
reply=$("$UMBRIEL" focused-window -j)
jq -e --arg id "$id_a" '.id == $id' > /dev/null <<< "$reply" || fail "reply is not fw-a: $reply"
entry=$(windows_json | jq -c --arg id "$id_a" '.[] | select(.id == $id)')
jq -e --argjson entry "$entry" '. == $entry' > /dev/null <<< "$reply" \
  || fail "reply differs from the windows entry: $reply vs $entry"

# The answer follows focus to fw-b.
"$UMBRIEL" msg "window-focus:$id_b" > /dev/null
for _ in $(seq 100); do
  reply=$("$UMBRIEL" focused-window -j)
  jq -e --arg id "$id_b" '.id == $id' > /dev/null <<< "$reply" && break
  sleep 0.05
done
jq -e --arg id "$id_b" '.id == $id' > /dev/null <<< "$reply" || fail "reply did not follow to fw-b: $reply"

# A layer-shell surface taking the keyboard (the panel case) borrows the seat
# without changing which window window-actions act on; the answer must hold
# through the takeover and after it ends.
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 keyboard=exclusive > "$UMBRIEL_RUNTIME_DIR/focused-window-layer.log" 2>&1 &
layer_pid=$!
for _ in $(seq 100); do
  "$UMBRIEL" layers --json | jq -e 'any(.[]; .mapped and .keyboard_interactivity == "exclusive")' > /dev/null \
    && break
  sleep 0.05
done
"$UMBRIEL" layers --json | jq -e 'any(.[]; .mapped and .keyboard_interactivity == "exclusive")' > /dev/null \
  || fail "the exclusive layer never mapped: $("$UMBRIEL" layers --json)"
"$UMBRIEL" settle > /dev/null
reply=$("$UMBRIEL" focused-window -j)
jq -e --arg id "$id_b" '.id == $id' > /dev/null <<< "$reply" \
  || fail "seat takeover changed the answer: $reply"

kill "$layer_pid"
wait "$layer_pid" || true
"$UMBRIEL" settle > /dev/null
reply=$("$UMBRIEL" focused-window -j)
jq -e --arg id "$id_b" '.id == $id' > /dev/null <<< "$reply" \
  || fail "answer changed after the layer went away: $reply"

# A scratchpad holding the seat with nothing focused answers "no focused window"
# rather than falling back to the workspace's remembered focus, which is the state
# the window actions treat as having no target. Hiding the pad restores the answer.
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" settle > /dev/null
if scratch_reply=$("$UMBRIEL" focused-window -j 2> "$UMBRIEL_RUNTIME_DIR/focused-window-scratch.err"); then
  fail "focused-window answered while a scratchpad held the seat: $scratch_reply"
fi
if ! grep -q "no focused window" "$UMBRIEL_RUNTIME_DIR/focused-window-scratch.err"; then
  fail "stderr lacks 'no focused window' with a scratchpad holding the seat: $(cat "$UMBRIEL_RUNTIME_DIR/focused-window-scratch.err")"
fi

"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" settle > /dev/null
reply=$("$UMBRIEL" focused-window -j)
jq -e --arg id "$id_b" '.id == $id' > /dev/null <<< "$reply" \
  || fail "answer did not return after the scratchpad went away: $reply"

# A scratchpad window that itself holds the keyboard answers as that window,
# the same target the window actions use. Only a pad holding the seat with no
# window focused is "no focused window".
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
for _ in $(seq 100); do
  windows_json | jq -e --arg id "$id_b" 'any(.[]; .id == $id and .scratchpad == "default")' > /dev/null && break
  sleep 0.05
done
windows_json | jq -e --arg id "$id_b" 'any(.[]; .id == $id and .scratchpad == "default")' > /dev/null \
  || fail "fw-b never reached the scratchpad: $(windows_json)"
"$UMBRIEL" msg scratchpad-toggle > /dev/null
for _ in $(seq 100); do
  windows_json | jq -e --arg id "$id_b" 'any(.[]; .id == $id and .active)' > /dev/null && break
  sleep 0.05
done
"$UMBRIEL" settle > /dev/null
if ! reply=$("$UMBRIEL" focused-window -j 2> "$UMBRIEL_RUNTIME_DIR/focused-window-scratchpad.err"); then
  fail "a focused scratchpad window got an error: $(cat "$UMBRIEL_RUNTIME_DIR/focused-window-scratchpad.err")"
fi
jq -e --arg id "$id_b" '.id == $id and .scratchpad == "default"' > /dev/null <<< "$reply" \
  || fail "a focused scratchpad window did not answer as itself: $reply"

echo "the focused window answers with the window-action target and its listing entry through seat takeovers"
