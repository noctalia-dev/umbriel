#!/usr/bin/env bash
# Closing the focused window while the overview is open focuses its nearest predecessor. The overview keeps the focus chrome while it owns the seat, so an unmap must move the workspace's focused view immediately: the card border is what shows where each row will land, and a dead focused view leaves no card highlighted until zoom-out happens to refocus. The closed window is closed through unmap-client, which unmaps on the close request without destroying the surface, so Server::removeView's destroy-time refocus can never mask a missing unmap-time reassignment.
set -euo pipefail

readonly UNMAP_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

spawn_survivor() {
  local title=$1
  "$UNMAP_CLIENT" "$title" > /dev/null 2>&1 &
}

wait_for_windows() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $want window(s), got $("$UMBRIEL" windows --json | jq 'length'): $("$UMBRIEL" windows --json)"
  return 1
}

# Two survivors map first so the unmap-client, mapping last, is the focused one. The second survivor is adjacent to it,
# which distinguishes the intended predecessor from an incorrect scan beginning at the first window.
spawn_survivor "overview-refocus-first"
wait_for_windows 1
spawn_survivor "overview-refocus-adjacent"
wait_for_windows 2

CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/unmap-client.log"
"$UNMAP_CLIENT" > "$CLIENT_LOG" 2>&1 &
UNMAP_PID=$!

for _ in $(seq 40); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "unmap-client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
wait_for_windows 3

unmap_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "unmap-client") | .id')
adjacent_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "overview-refocus-adjacent") | .id')
if [[ -z $unmap_id || -z $adjacent_id ]]; then
  echo "could not resolve the closing and adjacent window ids: $("$UMBRIEL" windows --json)"
  exit 1
fi

# The user-visible precondition: the newest window owns the focus.
if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$unmap_id" '.[] | select(.id == $id) | .focused') != true ]]; then
  echo "newly spawned unmap-client is not focused before the close"
  exit 1
fi

"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

"$UMBRIEL" msg "window-close:$unmap_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$CLIENT_LOG"; then
  echo "unmap-client never received the close request: $(cat "$CLIENT_LOG")"
  exit 1
fi
if ! kill -0 "$UNMAP_PID" 2>/dev/null; then
  echo "unmap-client exited instead of staying alive, the check cannot distinguish unmap from destroy"
  exit 1
fi

# Focus must move to the adjacent predecessor while the overview is still open.
focused=""
for _ in $(seq 40); do
  focused=$("$UMBRIEL" windows --json | jq -r --arg id "$adjacent_id" '.[] | select(.id == $id) | .focused')
  [[ $focused == true ]] && break
  sleep 0.1
done
if [[ $focused != true ]]; then
  echo "adjacent predecessor is not focused after closing the focused window in the overview: $("$UMBRIEL" windows --json)"
  exit 1
fi

# Zooming back in must land on the same window and keep it focused.
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle
if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$adjacent_id" '.[] | select(.id == $id) | .focused') != true ]]; then
  echo "adjacent predecessor lost focus after zooming out of the overview: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "closing the focused window in the overview focuses its adjacent predecessor"
