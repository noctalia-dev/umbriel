#!/usr/bin/env bash
# Closing a focused floating overlay must restore focus to the most recently focused window, not to whichever tiled
# view precedes the overlay in insertion order. The overlay is asked to close and the client only unmaps without
# destroying its surface, so no destroy-time cleanup can mask a missing unmap-time focus reassignment: the workspace
# must be refocused while the overlay's View still exists as registered-but-unmapped.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"

wait_for_window_count() {
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
    if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$want" '.[] | select(.id == $id) | .focused') == true ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $want to be focused: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
animation_ms = 1

[layout]
mode = "scrolling"

[[window_rule]]
match.app_id = "^float-close-overlay$"
default_floating = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" "float-close-anchor" > "$UMBRIEL_RUNTIME_DIR/anchor.log" 2>&1 &
wait_for_window_count 1
"$CLIENT" "float-close-neighbor" > "$UMBRIEL_RUNTIME_DIR/neighbor.log" 2>&1 &
wait_for_window_count 2

windows=$("$UMBRIEL" windows --json)
anchor_id=$(jq -r '.[] | select(.title == "float-close-anchor") | .id' <<< "$windows")
neighbor_id=$(jq -r '.[] | select(.title == "float-close-neighbor") | .id' <<< "$windows")
if [[ $(jq -r --arg id "$anchor_id" '.[] | select(.id == $id) | .floating' <<< "$windows") != false
    || $(jq -r --arg id "$neighbor_id" '.[] | select(.id == $id) | .floating' <<< "$windows") != false ]]; then
  echo "expected two tiles before opening the overlay: $windows"
  exit 1
fi

# Make the most recently focused view differ from the overlay's insertion-order predecessor: refocusing the anchor
# here means most-recently-focused order says anchor, while the reverse-insertion scan this check guards against
# would hand back the neighbor.
"$UMBRIEL" msg "window-focus:$anchor_id" > /dev/null
wait_for_focus "$anchor_id"

APP_ID=float-close-overlay "$CLIENT" "float-close-overlay" > "$UMBRIEL_RUNTIME_DIR/overlay.log" 2>&1 &
overlay_pid=$!
wait_for_window_count 3

windows=$("$UMBRIEL" windows --json)
overlay_id=$(jq -r '.[] | select(.title == "float-close-overlay") | .id' <<< "$windows")
if [[ $(jq -r --arg id "$overlay_id" '.[] | select(.id == $id) | .floating' <<< "$windows") != true ]]; then
  echo "overlay did not map floating: $windows"
  exit 1
fi
wait_for_focus "$overlay_id"

"$UMBRIEL" msg "window-close:$overlay_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/overlay.log" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/overlay.log"; then
  echo "overlay never unmapped in response to the close request: $(cat "$UMBRIEL_RUNTIME_DIR/overlay.log")"
  exit 1
fi
# The unmapped overlay drops out of `windows` while its process is still alive, so reaching a count of two proves the
# reassignment happened at unmap time with the View still registered. The overlay process must stay alive until after
# every assertion below: killing it destroys the View and Server::removeView's destroy-time refocus would mask a
# missing unmap-time reassignment. The very first observation must already show the anchor focused.
wait_for_window_count 2
windows=$("$UMBRIEL" windows --json)
if [[ $(jq -r --arg id "$anchor_id" '.[] | select(.id == $id) | .focused' <<< "$windows") != true ]]; then
  echo "focus was not restored to the previously focused window at unmap time: $windows"
  exit 1
fi

echo "closing a focused floating overlay restores the previously focused window"
