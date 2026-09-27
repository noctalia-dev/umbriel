#!/usr/bin/env bash
# Primary-selection visibility follows middle-click paste for new connections
# while staying stable for clients that remain connected across a reload.
set -euo pipefail

readonly GLOBAL_CLIENT="${UMBRIEL_GLOBAL_CLIENT:-./build-debug/tests/global-client}"
readonly PRIMARY_SELECTION=zwp_primary_selection_device_manager_v1
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/primary-selection-client.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/primary-selection-control"

if [[ ! -x $GLOBAL_CLIENT ]]; then
  echo "global client not built at $GLOBAL_CLIENT"
  exit 1
fi

query_global() {
  "$GLOBAL_CLIENT" "$PRIMARY_SELECTION" "$1"
}

query_global present

printf '\n[input]\nmiddle_click_paste = false\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
query_global absent

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
"$GLOBAL_CLIENT" recheck "$PRIMARY_SELECTION" absent <&"$control_fd" > "$CLIENT_LOG" 2>&1 &
client_pid=$!

for _ in $(seq 100); do
  grep -q '^ready$' "$CLIENT_LOG" && break
  if ! kill -0 "$client_pid" 2>/dev/null; then
    wait "$client_pid" 2>/dev/null || true
    echo "persistent global client exited before its recheck: $(< "$CLIENT_LOG")"
    exit 1
  fi
  sleep 0.02
done
if ! grep -q '^ready$' "$CLIENT_LOG"; then
  echo "persistent global client did not become ready: $(< "$CLIENT_LOG")"
  exit 1
fi

sed -i 's/middle_click_paste = false/middle_click_paste = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
query_global present

printf '\n' >&"$control_fd"
if ! wait "$client_pid"; then
  echo "primary-selection visibility changed for an existing client: $(< "$CLIENT_LOG")"
  exit 1
fi

echo "primary-selection visibility follows middle_click_paste and stays stable per client"
