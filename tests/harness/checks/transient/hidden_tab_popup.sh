#!/usr/bin/env bash
# A popup opened by a window hidden behind another tab in its column still gets its first configure. The client must
# not attach a buffer before it, and GTK 4 blocks waiting for it, so a missing configure freezes the whole client.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly POPUP_CLIENT="${UMBRIEL_POPUP_CLIENT:-./build-debug/tests/popup-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/hidden-tab-client.log"
readonly FIFO="$UMBRIEL_RUNTIME_DIR/hidden-tab.fifo"

if [[ ! -x $POPUP_CLIENT ]]; then
  echo "popup client is not built"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout.tabs]
default_display = "tabbed"

[[window_rule]]
match.title = "^popup-focus-regression$"
default_scrolling_column = "tabs"
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$FIFO"
"$POPUP_CLIENT" --popup-on-stdin < "$FIFO" > "$CLIENT_LOG" 2>&1 &
client_pid=$!
exec {popup_fd}> "$FIFO"
await_lines "$CLIENT_LOG" ready 1

# A second tab in the same column takes the front.
"$POPUP_CLIENT" > /dev/null 2>&1 &
# Not `umbriel settle`: the client acks later toplevel configures without committing, so it would never settle.
hidden='.[] | select(.pid == $pid) | .tab_hidden and (.visible | not)'
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  jq -e --argjson pid "$client_pid" "$hidden" <<< "$windows" > /dev/null && break
  sleep 0.1
done
if ! jq -e --argjson pid "$client_pid" "$hidden" <<< "$windows" > /dev/null; then
  echo "the popup client's window is not a hidden tab: $windows"
  exit 1
fi

echo popup >&"$popup_fd"
await_lines "$CLIENT_LOG" popup-requested 1
await_lines "$CLIENT_LOG" popup-mapped 1

echo "a popup of a hidden tab gets its first configure"
