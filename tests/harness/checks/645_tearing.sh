#!/usr/bin/env bash
# The tearing-control protocol is only a client hint. Umbriel must additionally require a fullscreen surface and the
# output opt-in. A per-window override can veto an async hint or force eligibility for a vsync-hinted client. The
# headless backend accepts the tearing state without performing a physical page flip, so this check observes the exact
# state Umbriel submitted through its diagnostics. Backend rejection and the synchronized retry are unit-tested.
set -euo pipefail

readonly GLOBAL_CLIENT="${UMBRIEL_GLOBAL_CLIENT:-./build-debug/global-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
CLIENT_PID=
CLIENT_LOG=

if [[ ! -x $GLOBAL_CLIENT || ! -x $CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

"$GLOBAL_CLIENT" wp_tearing_control_manager_v1 present 1

write_config() {
  printf '%s\n%s\n' "$BASELINE" "$1" > "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

wait_for_state() {
  local filter=$1
  local state=
  for _ in $(seq 100); do
    state=$("$UMBRIEL" tearing --json)
    if jq -e "$filter" <<< "$state" > /dev/null; then
      printf '%s\n' "$state"
      return 0
    fi
    sleep 0.05
  done
  printf 'tearing state did not settle for filter:\n%s\nlast state: %s\n' "$filter" "$state" >&2
  return 1
}

start_client() {
  local title=$1
  local hint=$2
  CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/$title.log"
  env TEARING_HINT="$hint" REQUEST_FULLSCREEN=1 REDRAW_ON_CLOSE=1 APP_ID="$title" \
    "$CLIENT" "$title" > "$CLIENT_LOG" 2>&1 &
  CLIENT_PID=$!
  for _ in $(seq 80); do
    grep -q '^mapped$' "$CLIENT_LOG" && return 0
    if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
      wait "$CLIENT_PID" 2>/dev/null || true
      echo "$title exited before mapping: $(< "$CLIENT_LOG")"
      return 1
    fi
    sleep 0.05
  done
  echo "$title did not map: $(< "$CLIENT_LOG")"
  return 1
}

redraw_client() {
  local before count
  before=$(grep -c '^redrawn$' "$CLIENT_LOG" || true)
  "$UMBRIEL" msg window-close > /dev/null
  for _ in $(seq 80); do
    count=$(grep -c '^redrawn$' "$CLIENT_LOG" || true)
    if ((count > before)); then
      return 0
    fi
    if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
      wait "$CLIENT_PID" 2>/dev/null || true
      echo "client exited before redraw: $(< "$CLIENT_LOG")"
      return 1
    fi
    sleep 0.05
  done
  echo "client did not redraw after close request: $(< "$CLIENT_LOG")"
  return 1
}

stop_client() {
  if [[ -n $CLIENT_PID ]]; then
    kill -TERM "$CLIENT_PID" 2>/dev/null || true
    wait "$CLIENT_PID" 2>/dev/null || true
    CLIENT_PID=
  fi
  wait_for_state '(.surfaces | length) == 0' > /dev/null
}

start_client tearing-async async
wait_for_state '
  (.surfaces | length) == 1
  and .surfaces[0].title == "tearing-async"
  and .surfaces[0].fullscreen == true
' > /dev/null
redraw_client

wait_for_state '
  .protocol == true
  and (.outputs | length) == 1
  and .outputs[0].name == "HEADLESS-1"
  and .outputs[0].allowed == false
  and .outputs[0].requested == false
  and .outputs[0].last_commit_tearing == false
  and .outputs[0].fallback_reason == ""
  and ((.outputs[0].last_presentation_presented == null) or ((.outputs[0].last_presentation_presented | type) == "boolean"))
  and ((.outputs[0].last_presentation_vsync == null) or ((.outputs[0].last_presentation_vsync | type) == "boolean"))
  and (.surfaces | length) == 1
  and .surfaces[0].title == "tearing-async"
  and .surfaces[0].hint == "async"
  and .surfaces[0].rule_override == null
  and .surfaces[0].fullscreen == true
  and .surfaces[0].eligible == false
' > /dev/null

write_config '[output.HEADLESS-1]
tearing = true'
redraw_client

wait_for_state '
  .outputs[0].allowed == true
  and .outputs[0].requested == true
  and .outputs[0].last_commit_tearing == true
  and .outputs[0].fallback_reason == ""
  and .surfaces[0].title == "tearing-async"
  and .surfaces[0].hint == "async"
  and .surfaces[0].rule_override == null
  and .surfaces[0].fullscreen == true
  and .surfaces[0].eligible == true
' > /dev/null

tearing_human=$("$UMBRIEL" tearing)
if ! grep -F "tearing control: yes" <<< "$tearing_human" > /dev/null \
    || ! grep -F "output HEADLESS-1: allowed yes, requested yes, last commit async" <<< "$tearing_human" > /dev/null \
    || ! grep -F "hint async, rule inherit, fullscreen yes, eligible yes" <<< "$tearing_human" > /dev/null; then
  echo "unexpected human-readable tearing diagnostics: $tearing_human"
  exit 1
fi

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_state '.surfaces[0].fullscreen == false' > /dev/null
redraw_client
wait_for_state '
  .outputs[0].allowed == true
  and .outputs[0].requested == false
  and .outputs[0].last_commit_tearing == false
  and .outputs[0].fallback_reason == ""
  and .surfaces[0].fullscreen == false
  and .surfaces[0].eligible == false
' > /dev/null

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_state '.surfaces[0].fullscreen == true' > /dev/null
redraw_client
wait_for_state '
  .outputs[0].requested == true
  and .outputs[0].last_commit_tearing == true
  and .surfaces[0].fullscreen == true
  and .surfaces[0].eligible == true
' > /dev/null

write_config '[output.HEADLESS-1]
tearing = true

[[window_rule]]
match.app_id = "^tearing-async$"
tearing = false'
redraw_client

wait_for_state '
  .outputs[0].allowed == true
  and .outputs[0].requested == false
  and .outputs[0].last_commit_tearing == false
  and .outputs[0].fallback_reason == ""
  and .surfaces[0].hint == "async"
  and .surfaces[0].rule_override == false
  and .surfaces[0].fullscreen == true
  and .surfaces[0].eligible == false
' > /dev/null

stop_client

write_config '[output.HEADLESS-1]
tearing = true

[[window_rule]]
match.app_id = "^tearing-forced$"
tearing = true'
start_client tearing-forced vsync
wait_for_state '
  .surfaces[0].title == "tearing-forced"
  and .surfaces[0].fullscreen == true
  and .surfaces[0].rule_override == true
' > /dev/null
redraw_client

wait_for_state '
  .outputs[0].allowed == true
  and .outputs[0].requested == true
  and .outputs[0].last_commit_tearing == true
  and .outputs[0].fallback_reason == ""
  and .surfaces[0].title == "tearing-forced"
  and .surfaces[0].hint == "vsync"
  and .surfaces[0].rule_override == true
  and .surfaces[0].fullscreen == true
  and .surfaces[0].eligible == true
' > /dev/null

stop_client
echo "tearing requires output opt-in and fullscreen, honors hints, and applies window overrides"
