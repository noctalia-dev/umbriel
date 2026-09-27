#!/usr/bin/env bash
# A parent request sent before the parent maps must still make the child open floating.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/unmapped-transient-parent.log"

wait_for_window_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

TRANSIENT_SUITE=unmapped-parent "$CLIENT" transient-child 600 500 > "$CLIENT_LOG" 2>&1 &
client_pid=$!

wait_for_window_count 2

windows=$("$UMBRIEL" windows --json)
if ! jq -e '
  length == 2
  and (.[] | select(.title == "transient-parent") | .floating) == false
  and (.[] | select(.title == "transient-child") | .floating) == true
' <<< "$windows" > /dev/null; then
  echo "an early-parented child did not open floating: $windows"
  echo "client log: $(< "$CLIENT_LOG")"
  exit 1
fi

kill "$client_pid"
wait "$client_pid" 2>/dev/null || true
wait_for_window_count 0

TRANSIENT_SUITE=unmapped-parent-cleared "$CLIENT" transient-child 600 500 > "$CLIENT_LOG" 2>&1 &
wait_for_window_count 2

windows=$("$UMBRIEL" windows --json)
if ! jq -e '
  length == 2
  and all(.[]; .floating == false)
' <<< "$windows" > /dev/null; then
  echo "clearing an early parent request did not leave the child tiled: $windows"
  echo "client log: $(< "$CLIENT_LOG")"
  exit 1
fi

echo "early parent requests float children and explicit cancellation keeps them tiled"
