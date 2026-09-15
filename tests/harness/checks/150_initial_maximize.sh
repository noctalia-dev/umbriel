#!/usr/bin/env bash
# Saved client maximization is ignored by default and honored only when configured.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

wait_for_log() {
  local log=$1 pattern=$2
  for _ in $(seq 40); do
    grep -q "$pattern" "$log" && return 0
    sleep 0.05
  done
  echo "maximize client did not report '$pattern': $(cat "$log")"
  return 1
}

spawn_maximized_client() {
  local title=$1 log=$2
  shift 2
  env LOG_CONFIGURES=1 "$@" "$CLIENT" "$title" > "$log" 2>&1 &
  CLIENT_PID=$!

  wait_for_log "$log" '^mapped$'
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
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/initial-maximize-control"
mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
env LOG_CONFIGURES=1 REQUEST_MAXIMIZED=1 REQUEST_MAXIMIZED_AFTER_MAP=1 MAXIMIZE_ON_STDIN=1 \
  "$CLIENT" initial-maximize-default <&"$control_fd" > "$DEFAULT_LOG" 2>&1 &
CLIENT_PID=$!
wait_for_log "$DEFAULT_LOG" '^mapped$' || exit 1

# Synchronize after the post-map restore re-assertion and end the opening
# sequence before issuing a distinct client maximize request.
printf s >&"$control_fd"
wait_for_log "$DEFAULT_LOG" '^surface-committed$' || exit 1
if grep -q '^configured-maximized$' "$DEFAULT_LOG"; then
  echo "opening client maximize request was accepted by default"
  exit 1
fi
printf m >&"$control_fd"
wait_for_log "$DEFAULT_LOG" '^maximize-requested$' || exit 1
for _ in $(seq 40); do
  request_line=$(grep -n '^maximize-requested$' "$DEFAULT_LOG" | sed -n '1s/:.*//p')
  maximize_line=$(grep -n '^configured-maximized$' "$DEFAULT_LOG" | sed -n '1s/:.*//p' || true)
  [[ -n $request_line && -n $maximize_line && $maximize_line -gt $request_line ]] && break
  sleep 0.05
done
if [[ -z ${maximize_line:-} || $maximize_line -le $request_line ]]; then
  echo "post-opening client maximize request was ignored: $(cat "$DEFAULT_LOG")"
  exit 1
fi
stop_client

printf '\nhonor_restored_maximize = true\n\n[animation]\nenabled = false\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

readonly HONORED_LOG="$UMBRIEL_RUNTIME_DIR/initial-maximize-honored.log"
spawn_maximized_client initial-maximize-honored "$HONORED_LOG" REQUEST_MAXIMIZED_AFTER_CONFIGURE=1 || exit 1
wait_for_log "$HONORED_LOG" '^configured-maximized$' || exit 1
assert_maximized_before_map "$HONORED_LOG"
stop_client


echo "opening maximize requests follow restore policy and later requests remain valid"
