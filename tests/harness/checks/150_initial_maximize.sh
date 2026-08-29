#!/usr/bin/env bash
# Saved client maximization is ignored by default and honored only when configured.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"

spawn_maximized_client() {
  local title=$1 log=$2
  shift 2
  env LOG_CONFIGURES=1 "$@" "$CLIENT" "$title" > "$log" 2>&1 &
  CLIENT_PID=$!

  for _ in $(seq 40); do
    grep -q '^mapped$' "$log" && return 0
    sleep 0.05
  done
  echo "maximize client never mapped: $(cat "$log")"
  return 1
}

assert_maximized_before_map() {
  local log=$1 maximize_line mapped_line
  maximize_line=$(grep -n '^configured-maximized$' "$log" | sed -n '1s/:.*//p')
  mapped_line=$(grep -n '^mapped$' "$log" | sed -n '1s/:.*//p')
  if [[ -z $maximize_line || -z $mapped_line || $maximize_line -ge $mapped_line ]]; then
    echo "restored maximize was not configured before the first buffer mapped"
    return 1
  fi
}

stop_client() {
  kill -KILL "$CLIENT_PID" 2>/dev/null || true
  wait "$CLIENT_PID" 2>/dev/null || true
}

readonly DEFAULT_LOG="$UMBRIEL_RUNTIME_DIR/initial-maximize-default.log"
spawn_maximized_client initial-maximize-default "$DEFAULT_LOG" REQUEST_MAXIMIZED=1 || exit 1
sleep 0.3
if grep -q '^configured-maximized$' "$DEFAULT_LOG"; then
  echo "opening client maximize request was accepted by default"
  exit 1
fi
# The honored phase reuses CLIENT_PID for its own client, so the default-config
# one is stopped while it can still be signalled.
stop_client

printf '\nhonor_restored_maximize = true\n\n[animation]\nenabled = false\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

readonly HONORED_LOG="$UMBRIEL_RUNTIME_DIR/initial-maximize-honored.log"
spawn_maximized_client initial-maximize-honored "$HONORED_LOG" REQUEST_MAXIMIZED_AFTER_CONFIGURE=1 || exit 1
sleep 0.3
if ! grep -q '^configured-maximized$' "$HONORED_LOG"; then
  echo "configured opening client maximize request was ignored"
  exit 1
fi
assert_maximized_before_map "$HONORED_LOG"
stop_client


echo "opening maximize requests before the first buffer are resolved in the initial configure"
