#!/usr/bin/env bash
# harness: outputs=2
# A pointer warp that an action causes is not user activity: an idle notification stays idle across
# workspace-switch --output, which warps the cursor to the other output.
set -euo pipefail

readonly CLIENT="${UMBRIEL_IDLE_NOTIFY_CLIENT:-./build-debug/tests/idle-notify-client}"
CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/idle-notify-client.log"

cat >> "$UMBRIEL_CONFIG" <<'CFG'

[output.HEADLESS-1]
mode = "1280x720"
position = [0, 0]
workspaces = ["LEFT"]

[output.HEADLESS-2]
mode = "1280x720"
position = [1280, 0]
workspaces = ["RIGHT"]
CFG
"$UMBRIEL" msg config-reload > /dev/null

wait_for() {
  for _ in $(seq 60); do
    grep -q "^$1\$" "$CLIENT_LOG" && return 0
    sleep 0.05
  done
  return 1
}

"$CLIENT" 200 > "$CLIENT_LOG" 2>&1 &
wait_for ready || { echo "idle notify client never became ready: $(cat "$CLIENT_LOG")"; exit 1; }
wait_for idled || { echo "client never went idle: $(cat "$CLIENT_LOG")"; exit 1; }

for output in HEADLESS-2 HEADLESS-1; do
  "$UMBRIEL" msg workspace-switch "$([[ $output == HEADLESS-2 ]] && echo RIGHT || echo LEFT)" --output "$output" > /dev/null
  sleep 0.3 # real time: a spurious resume arrives within a dispatch
  if grep -q '^resumed$' "$CLIENT_LOG"; then
    echo "workspace-switch --output $output resumed the idle client: $(cat "$CLIENT_LOG")"
    exit 1
  fi
done

echo "action-initiated cursor warps leave the idle notification idle"
