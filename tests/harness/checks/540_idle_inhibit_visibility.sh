#!/usr/bin/env bash
# A visible toplevel inhibits idle, then stops inhibiting as soon as its
# workspace becomes inactive while its process and protocol objects stay alive.
set -euo pipefail

readonly CLIENT="${UMBRIEL_IDLE_INHIBIT_CLIENT:-./build-debug/tests/idle-inhibit-client}"
CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/idle-inhibit-client.log"

"$CLIENT" > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 40); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "idle inhibitor client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

sleep 0.35 # real time: outlast the idle timeout
if grep -q '^idled$' "$CLIENT_LOG"; then
  echo "visible surface failed to inhibit idle"
  exit 1
fi

"$UMBRIEL" msg workspace-switch:2 > /dev/null
for _ in $(seq 40); do
  grep -q '^idled$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^idled$' "$CLIENT_LOG"; then
  echo "hidden surface continued to inhibit idle: $(cat "$CLIENT_LOG")"
  exit 1
fi

echo "idle inhibitor follows toplevel workspace visibility"
