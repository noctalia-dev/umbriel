#!/usr/bin/env bash
# Decoration manager visibility follows prefer_no_csd for new clients while
# staying stable for clients that remain connected across a config reload.
set -euo pipefail

readonly GLOBAL_CLIENT="${UMBRIEL_GLOBAL_CLIENT:-./build-debug/tests/global-client}"
readonly XDG_DECORATION=zxdg_decoration_manager_v1
readonly KDE_DECORATION=org_kde_kwin_server_decoration_manager
readonly BIND_LOG="$UMBRIEL_RUNTIME_DIR/decoration-bind-client.log"
readonly RECHECK_LOG="$UMBRIEL_RUNTIME_DIR/decoration-recheck-client.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/decoration-control"

if [[ ! -x $GLOBAL_CLIENT ]]; then
  echo "global client not built at $GLOBAL_CLIENT"
  exit 1
fi

query_decoration_globals() {
  "$GLOBAL_CLIENT" "$XDG_DECORATION" "$1"
  "$GLOBAL_CLIENT" "$KDE_DECORATION" "$1"
}

wait_for_ready() {
  local pid=$1 log=$2 description=$3
  for _ in $(seq 100); do
    grep -q '^ready$' "$log" && return 0
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      echo "$description exited before becoming ready: $(< "$log")"
      return 1
    fi
    sleep 0.02
  done
  echo "$description did not become ready: $(< "$log")"
  return 1
}

query_decoration_globals present

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
"$GLOBAL_CLIENT" bind "$XDG_DECORATION" present <&"$control_fd" > "$BIND_LOG" 2>&1 &
bind_pid=$!
wait_for_ready "$bind_pid" "$BIND_LOG" "binding global client"

printf '\n[appearance]\nprefer_no_csd = false\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
query_decoration_globals absent

printf '\n' >&"$control_fd"
if ! wait "$bind_pid"; then
  echo "an advertised decoration global could not be bound after reload: $(< "$BIND_LOG")"
  exit 1
fi

"$GLOBAL_CLIENT" recheck "$XDG_DECORATION" absent <&"$control_fd" > "$RECHECK_LOG" 2>&1 &
recheck_pid=$!
wait_for_ready "$recheck_pid" "$RECHECK_LOG" "rechecking global client"

sed -i 's/prefer_no_csd = false/prefer_no_csd = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
query_decoration_globals present

printf '\n' >&"$control_fd"
if ! wait "$recheck_pid"; then
  echo "decoration visibility changed for an existing client: $(< "$RECHECK_LOG")"
  exit 1
fi

echo "decoration globals follow prefer_no_csd and remain bindable and stable per client"
