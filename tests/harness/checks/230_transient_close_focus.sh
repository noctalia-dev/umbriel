#!/usr/bin/env bash
# Closing a focused transient toplevel must restore focus to its parent. The unrelated tiled view maps between the
# parent and child, so insertion-order fallback picks the wrong view unless parentage wins.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/transient-client.log"

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

[animation]
duration_ms = 1

[layout]
mode = "scrolling"

[layout.scrolling]
default_width_fraction = 0.75
EOF
"$UMBRIEL" msg config-reload > /dev/null

TRANSIENT_SUITE=1 "$CLIENT" "transient-child" 600 500 > "$CLIENT_LOG" 2>&1 &
client_pid=$!

for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "transient child never mapped: $(< "$CLIENT_LOG")"
  exit 1
fi
wait_for_window_count 3

windows=$("$UMBRIEL" windows --json)
parent_id=$(jq -r '.[] | select(.title == "transient-parent") | .id' <<< "$windows")
unrelated_id=$(jq -r '.[] | select(.title == "transient-unrelated") | .id' <<< "$windows")
child_id=$(jq -r '.[] | select(.title == "transient-child") | .id' <<< "$windows")
if [[ -z $parent_id || -z $unrelated_id || -z $child_id ]]; then
  echo "could not resolve transient-suite window ids: $windows"
  exit 1
fi
if [[ $(jq -r --arg id "$parent_id" '.[] | select(.id == $id) | .floating' <<< "$windows") != false
    || $(jq -r --arg id "$unrelated_id" '.[] | select(.id == $id) | .floating' <<< "$windows") != false
    || $(jq -r --arg id "$child_id" '.[] | select(.id == $id) | .floating' <<< "$windows") != true ]]; then
  echo "transient suite did not map as two tiles and one floating child: $windows"
  exit 1
fi
if [[ $(jq -r --arg id "$child_id" '.[] | select(.id == $id) | .focused' <<< "$windows") != true ]]; then
  echo "transient child is not focused before close: $windows"
  exit 1
fi
"$UMBRIEL" msg "window-close:$child_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$CLIENT_LOG"; then
  echo "transient child did not unmap: $(< "$CLIENT_LOG")"
  exit 1
fi
if ! kill -0 "$client_pid" 2>/dev/null; then
  echo "transient client exited instead of keeping the child alive"
  exit 1
fi

wait_for_focus "$parent_id"
sleep 0.1
windows=$("$UMBRIEL" windows --json)
if [[ $(jq -r --arg id "$unrelated_id" '.[] | select(.id == $id) | .focused' <<< "$windows") != false ]]; then
  echo "unrelated view received focus after the transient closed: $windows"
  exit 1
fi

echo "closing a transient toplevel restores its parent instead of an unrelated view"
