#!/usr/bin/env bash
# A popup opened by a window on a workspace that is not shown still gets its first configure. The client must not
# attach a buffer before it, and GTK 4 blocks waiting for it, so a missing configure freezes the whole client.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly POPUP_CLIENT="${UMBRIEL_POPUP_CLIENT:-./build-debug/tests/popup-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/hidden-popup-client.log"
readonly FIFO="$UMBRIEL_RUNTIME_DIR/hidden-popup.fifo"

if [[ ! -x $POPUP_CLIENT ]]; then
  echo "popup client is not built"
  exit 1
fi

mkfifo "$FIFO"
"$POPUP_CLIENT" --popup-on-stdin < "$FIFO" > "$CLIENT_LOG" 2>&1 &
client_pid=$!
exec {popup_fd}> "$FIFO"
await_lines "$CLIENT_LOG" ready 1

"$UMBRIEL" msg "window-move-to-workspace-silent:2" > /dev/null
# Not `umbriel settle`: the client acks later toplevel configures without committing, so it would never settle.
hidden='.[] | select(.pid == $pid) | .visible == false'
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  jq -e --argjson pid "$client_pid" "$hidden" <<< "$windows" > /dev/null && break
  sleep 0.1
done
if ! jq -e --argjson pid "$client_pid" "$hidden" <<< "$windows" > /dev/null; then
  echo "the popup client's window is still shown after moving to workspace 2: $windows"
  exit 1
fi

echo popup >&"$popup_fd"
await_lines "$CLIENT_LOG" popup-requested 1
await_lines "$CLIENT_LOG" popup-mapped 1

echo "a popup of a window on a hidden workspace gets its first configure"
