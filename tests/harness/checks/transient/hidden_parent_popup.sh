#!/usr/bin/env bash
# A popup opened by a window that is not shown still gets its first configure, whether the window is on a workspace
# that is not shown or behind another tab in its column. The client must not attach a buffer before it, and GTK 4
# blocks waiting for it, so a missing configure freezes the whole client.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly POPUP_CLIENT="${UMBRIEL_POPUP_CLIENT:-./build-debug/tests/popup-client}"

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

# Starts a client that opens a popup on request and leaves its pid in $client_pid and its request fd in $popup_fd.
spawn_popup_client() {
  local name=$1
  mkfifo "$UMBRIEL_RUNTIME_DIR/$name.fifo"
  "$POPUP_CLIENT" --popup-on-stdin < "$UMBRIEL_RUNTIME_DIR/$name.fifo" > "$UMBRIEL_RUNTIME_DIR/$name.log" 2>&1 &
  client_pid=$!
  exec {popup_fd}> "$UMBRIEL_RUNTIME_DIR/$name.fifo"
  await_lines "$UMBRIEL_RUNTIME_DIR/$name.log" ready 1
}

# Not `umbriel settle`: the client acks later toplevel configures without committing, so it would never settle.
await_hidden() {
  local description=$1 windows=
  for _ in $(seq 60); do
    windows=$("$UMBRIEL" windows --json)
    jq -e --argjson pid "$client_pid" '.[] | select(.pid == $pid) | .visible == false' <<< "$windows" > /dev/null \
      && return 0
    sleep 0.1
  done
  echo "the popup client's window is not $description: $windows"
  return 1
}

await_popup_configure() {
  local name=$1
  echo popup >&"$popup_fd"
  await_lines "$UMBRIEL_RUNTIME_DIR/$name.log" popup-requested 1
  await_lines "$UMBRIEL_RUNTIME_DIR/$name.log" popup-mapped 1
}

spawn_popup_client workspace
"$UMBRIEL" msg "window-move-to-workspace-silent:2" > /dev/null
await_hidden "on a hidden workspace"
await_popup_configure workspace

spawn_popup_client tab
# A second tab in the same column takes the front.
"$POPUP_CLIENT" > /dev/null 2>&1 &
await_hidden "a hidden tab"
await_popup_configure tab

echo "popups of windows on a hidden workspace and behind another tab get their first configure"
