#!/usr/bin/env bash
# Public portal requests and backend-boundary input run in a private desktop.
set -Eeuo pipefail
trap 'echo "global shortcut frontend check failed at line $LINENO: $BASH_COMMAND" >&2' ERR
umask 077

BUILD=$(realpath "${1:?usage: global_shortcuts_portal.sh UMBRIEL_BUILD PORTAL_BUILD XDG_DESKTOP_PORTAL}")
PORTAL_BUILD=$(realpath "${2:?portal build directory required}")
FRONTEND=$(realpath "${3:?xdg-desktop-portal executable required}")
PERMISSION_STORE=$(dirname "$FRONTEND")/xdg-permission-store
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
readonly BUILD PORTAL_BUILD FRONTEND PERMISSION_STORE REPO
readonly PORTAL_REPO=${UMBRIEL_PORTAL_REPO:-$(dirname "$REPO")/xdg-desktop-portal-umbriel}
readonly UMBRIEL=$BUILD/umbriel
readonly APPLICATION=org.umbriel.GlobalShortcutsTest
for program in "$FRONTEND" "$PERMISSION_STORE" "$UMBRIEL" \
  "$BUILD/tests/input-capture-compositor" "$PORTAL_BUILD/xdg-desktop-portal-umbriel" \
  "$PORTAL_BUILD/global-shortcuts-test-client"; do
  [[ -x $program ]] || { echo "required executable missing: $program" >&2; exit 2; }
done
RUNTIME=$(mktemp -d /tmp/umbriel-shortcuts-portal.XXXXXXXX)
readonly RUNTIME
PIDS=()
SUCCESS=0
cleanup() {
  local pid
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
  if ((SUCCESS)) && [[ ${UMBRIEL_SHORTCUTS_KEEP_LOGS:-0} != 1 ]]; then
    rm -rf "$RUNTIME"
  else
    echo "global shortcut frontend logs retained in $RUNTIME"
  fi
}
trap cleanup EXIT
forget_pid() {
  local index
  for index in "${!PIDS[@]}"; do
    if [[ ${PIDS[index]} == "$1" ]]; then
      unset 'PIDS[index]'
      return 0
    fi
  done
  echo "process was not tracked: $1" >&2
  return 1
}
stop_process() {
  local pid=$1 status=0
  kill "$pid"
  wait "$pid" || status=$?
  forget_pid "$pid"
  if ((status != 0 && status != 143)); then
    echo "process $pid failed during shutdown: $status" >&2
    return 1
  fi
}

export XDG_RUNTIME_DIR=$RUNTIME XDG_CONFIG_HOME=$RUNTIME/config XDG_CONFIG_DIRS=$RUNTIME/config-dirs
export XDG_STATE_HOME=$RUNTIME/state XDG_CACHE_HOME=$RUNTIME/cache
export XDG_DATA_HOME=$RUNTIME/data-home XDG_DATA_DIRS=$RUNTIME/data
export XDG_CURRENT_DESKTOP=umbriel XDG_SESSION_TYPE=wayland GSETTINGS_BACKEND=memory NO_AT_BRIDGE=1
export DBUS_SESSION_BUS_ADDRESS=unix:path=$RUNTIME/bus DBUS_SYSTEM_BUS_ADDRESS=unix:path=$RUNTIME/no-system-bus
unset DISPLAY WAYLAND_SOCKET NOTIFY_SOCKET SYSTEMD_EXEC_PID
mkdir -p "$XDG_CONFIG_HOME/xdg-desktop-portal" "$XDG_DATA_HOME/applications" "$RUNTIME/portals"
cp "$PORTAL_REPO/data/umbriel.portal" "$RUNTIME/portals/umbriel.portal"
export XDG_DESKTOP_PORTAL_DIR=$RUNTIME/portals
cat > "$XDG_CONFIG_HOME/xdg-desktop-portal/portals.conf" <<'EOF'
[preferred]
default=umbriel
org.freedesktop.impl.portal.GlobalShortcuts=umbriel
EOF
cat > "$XDG_DATA_HOME/applications/$APPLICATION.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Global Shortcuts Test
Exec=$PORTAL_BUILD/global-shortcuts-test-client
EOF
cat > "$RUNTIME/config.toml" <<EOF
[general]
xwayland = false
show_cheatsheet = false
autostart = []
[animation]
enabled = false
[input.keyboard]
layout = "us"
numlock_toggle = false
[include]
files = ["$RUNTIME/binds.toml"]
EOF
: > "$RUNTIME/binds.toml"
# No service directories: only explicitly launched processes can own services.
cat > "$RUNTIME/bus.conf" <<EOF
<busconfig>
  <type>session</type>
  <listen>$DBUS_SESSION_BUS_ADDRESS</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow own="*"/>
    <allow send_destination="*"/>
    <allow receive_sender="*"/>
  </policy>
</busconfig>
EOF
dbus-daemon --nofork --config-file="$RUNTIME/bus.conf" > "$RUNTIME/bus.log" 2>&1 &
PIDS+=("$!")
# shellcheck source=tests/harness/lib.sh
source "$REPO/tests/harness/lib.sh"
wait_owner() {
  local name=$1 present=${2:-true}
  for _ in $(seq 200); do
    if dbus-send --session --print-reply --reply-timeout=1000 --dest=org.freedesktop.DBus \
      /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner "string:$name" 2>/dev/null | grep -q "boolean $present"; then
      return 0
    fi
    sleep 0.025
  done
  echo "bus service ownership did not become $present: $name" >&2
  return 1
}
wait_owner org.freedesktop.DBus

mkfifo "$RUNTIME/physical" "$RUNTIME/client"
exec {physical_fd}<> "$RUNTIME/physical"
exec {client_fd}<> "$RUNTIME/client"
start_compositor() {
  COMPOSITOR_LOG=$RUNTIME/compositor-$1.log
  WAYLAND_DISPLAY=shortcuts-portal-isolated-parent WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    "$BUILD/tests/input-capture-compositor" "$RUNTIME/config.toml" <&"$physical_fd" > "$COMPOSITOR_LOG" 2>&1 &
  compositor_pid=$!
  PIDS+=("$compositor_pid")
  await_events "$COMPOSITOR_LOG" 'ready display=' 1 "private compositor $1"
  export WAYLAND_DISPLAY UMBRIEL_SOCKET
  WAYLAND_DISPLAY=$(sed -n 's/^ready display=\([^ ]*\).*/\1/p' "$COMPOSITOR_LOG" | head -1)
  UMBRIEL_SOCKET=$(sed -n 's/^ready .*socket=\([^ ]*\).*/\1/p' "$COMPOSITOR_LOG" | head -1)
}
start_backend() {
  local label=$1
  backend_logger_pid=
  if [[ ${2:-} == fail-writes ]]; then
    # Keep logs outside the limit and make failed file writes return errors.
    mkfifo "$RUNTIME/backend-$label.pipe"
    cat "$RUNTIME/backend-$label.pipe" > "$RUNTIME/backend-$label.log" &
    backend_logger_pid=$!
    PIDS+=("$backend_logger_pid")
    bash -c 'trap "" XFSZ; ulimit -Sf 0 || exit; exec "$@"' _ \
      "$PORTAL_BUILD/xdg-desktop-portal-umbriel" > "$RUNTIME/backend-$label.pipe" 2>&1 &
  else
    "$PORTAL_BUILD/xdg-desktop-portal-umbriel" > "$RUNTIME/backend-$label.log" 2>&1 &
  fi
  backend_pid=$!
  PIDS+=("$backend_pid")
  wait_owner org.freedesktop.impl.portal.desktop.umbriel
}
start_frontend() {
  "$FRONTEND" > "$RUNTIME/frontend-$1.log" 2>&1 &
  frontend_pid=$!
  PIDS+=("$frontend_pid")
  wait_owner org.freedesktop.portal.Desktop
}
start_client() {
  CLIENT_LOG=$RUNTIME/client-$1.jsonl
  CLIENT_ERR=$RUNTIME/client-$1.err
  "$PORTAL_BUILD/global-shortcuts-test-client" "$APPLICATION" <&"$client_fd" > "$CLIENT_LOG" 2> "$CLIENT_ERR" &
  client_pid=$!
  PIDS+=("$client_pid")
}
start_compositor core
"$PERMISSION_STORE" > "$RUNTIME/permission-store.log" 2>&1 &
PIDS+=("$!")
wait_owner org.freedesktop.impl.portal.PermissionStore
start_backend core
start_frontend core
start_client core

send() { printf '%s\n' "$@" >&"$client_fd"; }
physical() { printf '%s\n' "$@" >&"$physical_fd"; }
expect_json() {
  if ! jq -se "$1" "$CLIENT_LOG" > /dev/null; then
    echo "public event assertion failed: $1" >&2
    return 1
  fi
}
await_json() {
  local expression=$1 label=$2
  for _ in $(seq 200); do
    expect_json "$expression" 2>/dev/null && return 0
    kill -0 "$client_pid" 2>/dev/null || break
    sleep 0.025
  done
  echo "timed out: $label" >&2
  cat "$CLIENT_LOG" "$CLIENT_ERR" >&2
  return 1
}
response() { await_json "any(.[]; .event == \"response\" and .tag == \"$1\" and .status == $2)" "$1 response $2"; }
roundtrip_serial=0
roundtrip() {
  local session=${2:-main}
  roundtrip_serial=$((roundtrip_serial + 1))
  physical "mark public-$roundtrip_serial"
  await_events "$COMPOSITOR_LOG" "mark public-$roundtrip_serial$" 1 "physical input barrier"
  send "{\"op\":\"list\",\"session\":\"$session\",\"tag\":\"$1\"}"
  response "$1" 0
}
status_serial=0
review() {
  local visible=$1
  for _ in $(seq 100); do
    status_serial=$((status_serial + 1))
    physical "shortcut-status $status_serial"
    await_events "$COMPOSITOR_LOG" "shortcut-status tag=$status_serial " 1 "approval status"
    if grep -q "^shortcut-status tag=$status_serial visible=$visible " "$COMPOSITOR_LOG"; then return 0; fi
    sleep 0.025
  done
  echo "approval visibility did not become $visible" >&2
  return 1
}
no_live_sessions() {
  for _ in $(seq 200); do
    "$UMBRIEL" global-shortcuts --json > "$RUNTIME/inspection.json"
    jq -e 'all(.[]; .live == false)' "$RUNTIME/inspection.json" > /dev/null && return 0
    sleep 0.025
  done
  echo "frontend retained a live compositor session" >&2
  return 1
}
finish_client() {
  send '{"op":"quit"}'
  wait "$client_pid"
  forget_pid "$client_pid"
  no_live_sessions
}
client_ready() {
  await_json 'any(.[]; .event == "ready" and .version == 1 and .application == "org.umbriel.GlobalShortcutsTest")' \
    "public version and registered identity"
}

client_ready
send '{"op":"create","tag":"create"}'
response create 0
roundtrip before-bind
expect_json 'any(.[]; .tag == "before-bind" and .event == "response" and .shortcuts == [])'
send '{"op":"bind","tag":"bind","shortcuts":[{"id":"talk","description":"Talk \"quoted\"\nnext line","preferred_trigger":"CTRL+m"}]}'
review true
physical 'tap 28'
response bind 0
expect_json 'any(.[]; .tag == "bind" and .event == "response" and .shortcuts ==
  [{"id":"talk","description":"Talk \"quoted\"\nnext line","trigger_description":"Ctrl+M"}])'
review false
roundtrip after-bind
expect_json '([.[] | select(.event == "response" and (.tag == "bind" or .tag == "after-bind")) | .shortcuts] | unique | length) == 1'
jq -s '[.[] | select(.event == "response" and .tag == "after-bind")][0].shortcuts' \
  "$CLIENT_LOG" > "$RUNTIME/approved-shortcuts.json"

# Physical input must arrive as one paired public activation with millisecond times.
physical 'press 29' 'tap 50' 'release 29'
roundtrip activated
expect_json '([.[] | select(.event == "activated")] | length) == 1 and
  ([.[] | select(.event == "deactivated")] | length) == 1 and
  all(.[] | select(.event == "activated" or .event == "deactivated"); .id == "talk" and .session == "main" and (.time | type) == "number") and
  ([.[] | select(.event == "activated" or .event == "deactivated") | .time] | .[1] == .[0] + 1)'
send '{"op":"configure"}' '{"op":"bind","tag":"repeat","shortcuts":[]}'
await_json 'any(.[]; .event == "error" and .method == "ConfigureShortcuts" and .error == "org.freedesktop.DBus.Error.UnknownMethod")' \
  "unsupported public editor"
response repeat 2
roundtrip preserved
expect_json 'any(.[]; .event == "response" and .tag == "preserved" and (.shortcuts | length) == 1)'

# Configured keybind priority publishes a complete empty replacement, then recovery.
changes_before=$(jq -s '[.[] | select(.event == "changed" and .session == "main")] | length' "$CLIENT_LOG")
cat > "$RUNTIME/binds.toml" <<'EOF'
[keybinds]
"Ctrl+M" = "workspace-set-layout dwindle"
EOF
"$UMBRIEL" msg config-reload > /dev/null
await_json "([.[] | select(.event == \"changed\" and .session == \"main\")] |
  length > $changes_before and .[-1].shortcuts == [])" "empty changed snapshot"
roundtrip reserved
expect_json 'any(.[]; .event == "response" and .tag == "reserved" and .shortcuts == [])'
changes_before=$(jq -s '[.[] | select(.event == "changed" and .session == "main")] | length' "$CLIENT_LOG")
: > "$RUNTIME/binds.toml"
"$UMBRIEL" msg config-reload > /dev/null
await_json "([.[] | select(.event == \"changed\" and .session == \"main\")] |
  length > $changes_before and (.[-1].shortcuts | length) == 1)" "restored changed snapshot"
expect_json '([.[] | select(.event == "changed" and .session == "main")][-1].shortcuts) ==
  ([.[] | select(.event == "response" and .tag == "after-bind")][0].shortcuts)'

# A cancelled new-ID review closes its session without disturbing the bound owner.
send '{"op":"create","session":"pending","tag":"pending-create"}'
response pending-create 0
send '{"op":"bind","session":"pending","tag":"pending-bind","shortcuts":[{"id":"other","description":"Other","preferred_trigger":"CTRL+n"}]}'
review true
send '{"op":"cancel"}'
await_json 'any(.[]; .event == "cancelled" and .tag == "pending-bind")' "public request cancellation"
await_json 'any(.[]; .event == "closed" and .session == "pending")' "cancelled session closure"
review false
physical 'press 29' 'tap 50' 'release 29'
roundtrip after-cancel
expect_json '([.[] | select(.event == "activated")] | length) == 2 and
  ([.[] | select(.event == "deactivated")] | length) == 2'
send '{"op":"close"}'
no_live_sessions

# A restored session left open on peer exit must also disappear from the compositor.
send '{"op":"create","session":"disconnect","tag":"disconnect-create"}'
response disconnect-create 0
finish_client
expect_json 'all(.[]; .event != "error" or .method == "ConfigureShortcuts")'

readonly CREDENTIAL_CACHE=$XDG_STATE_HOME/xdg-desktop-portal-umbriel/global-shortcuts-credentials.json
expect_restored_snapshot() {
  if ! jq -se --arg tag "$1" --slurpfile expected "$RUNTIME/approved-shortcuts.json" \
    'any(.[]; .event == "response" and .tag == $tag and .shortcuts == $expected[0])' "$CLIENT_LOG" > /dev/null; then
    echo "restored snapshot mismatch: $2" >&2
    return 1
  fi
}
restore_after_restart() {
  local label=$1
  cp "$CREDENTIAL_CACHE" "$RUNTIME/cache-before-$label.json"
  start_client "$label"
  client_ready
  send '{"op":"create","tag":"restore-create"}'
  response restore-create 0
  roundtrip restore-list
  expect_restored_snapshot restore-list "$label"
  if cmp -s "$CREDENTIAL_CACHE" "$RUNTIME/cache-before-$label.json"; then
    echo "restored credential was not rotated: $label" >&2
    return 1
  fi
  review false
  send '{"op":"bind","tag":"restore-bind","shortcuts":[{"id":"talk","description":"Talk \"quoted\"\nnext line","preferred_trigger":"CTRL+m"}]}'
  response restore-bind 0
  expect_restored_snapshot restore-bind "$label"
  review false
  physical 'press 29' 'tap 50' 'release 29'
  roundtrip restore-activated
  expect_json '([.[] | select(.event == "activated" and .session == "main" and .id == "talk")] | length) == 1 and
    ([.[] | select(.event == "deactivated" and .session == "main" and .id == "talk")] | length) == 1 and
    all(.[]; .event != "error")'
  finish_client
}

# Restart each service independently before discarding all compositor memory.
stop_process "$backend_pid"
wait_owner org.freedesktop.impl.portal.desktop.umbriel false
start_backend backend-restart
restore_after_restart backend
stop_process "$frontend_pid"
wait_owner org.freedesktop.portal.Desktop false
start_frontend frontend-restart
restore_after_restart frontend

stop_process "$frontend_pid"
wait_owner org.freedesktop.portal.Desktop false
stop_process "$backend_pid"
wait_owner org.freedesktop.impl.portal.desktop.umbriel false
physical quit
wait "$compositor_pid"
forget_pid "$compositor_pid"
start_compositor restarted
start_backend compositor-restart
start_frontend compositor-restart
restore_after_restart compositor

# A restore rotation cannot succeed if its replacement credential cannot be saved.
stop_process "$backend_pid"
wait_owner org.freedesktop.impl.portal.desktop.umbriel false
start_backend cache-failure fail-writes
cp "$CREDENTIAL_CACHE" "$RUNTIME/cache-before-failure.json"
start_client cache-failure
client_ready
send '{"op":"create","tag":"failed-restore"}'
response failed-restore 2
no_live_sessions
cmp "$CREDENTIAL_CACHE" "$RUNTIME/cache-before-failure.json"

# Without a cached credential, physical approval still cannot bypass a failed save.
mv "$CREDENTIAL_CACHE" "$RUNTIME/stale-cache.json"
send '{"op":"create","session":"failed-bind","tag":"failure-create"}'
response failure-create 0
send '{"op":"bind","session":"failed-bind","tag":"failed-bind","shortcuts":[{"id":"talk","description":"Talk \"quoted\"\nnext line","preferred_trigger":"CTRL+m"}]}'
review true
physical 'tap 28'
response failed-bind 2
await_json 'any(.[]; .event == "closed" and .session == "failed-bind")' "failed-save session closure"
no_live_sessions
review false
if [[ -e $CREDENTIAL_CACHE ]]; then
  echo "failed save published a credential cache" >&2
  exit 1
fi
physical 'press 29' 'tap 50' 'release 29' 'mark failed-save-input'
await_events "$COMPOSITOR_LOG" 'mark failed-save-input$' 1 "input after failed save"
send '{"op":"create","session":"barrier","tag":"failure-barrier-create"}'
response failure-barrier-create 0
send '{"op":"list","session":"barrier","tag":"failure-barrier-list"}'
response failure-barrier-list 0
expect_json 'all(.[]; .event != "activated" and .event != "deactivated" and .event != "changed" and .event != "error") and
  any(.[]; .event == "response" and .tag == "failure-barrier-list" and .shortcuts == [])'
finish_client
stop_process "$backend_pid"
wait_owner org.freedesktop.impl.portal.desktop.umbriel false
wait "$backend_logger_pid"
forget_pid "$backend_logger_pid"

# A healthy backend needs new consent after the unsaved grant, then remains usable.
start_backend cache-recovery
start_client cache-recovery
client_ready
send '{"op":"create","tag":"recovery-create"}'
response recovery-create 0
send '{"op":"bind","tag":"recovery-bind","shortcuts":[{"id":"talk","description":"Talk \"quoted\"\nnext line","preferred_trigger":"CTRL+m"}]}'
review true
physical 'tap 28'
response recovery-bind 0
expect_restored_snapshot recovery-bind cache-recovery
[[ -s $CREDENTIAL_CACHE ]]
physical 'press 29' 'tap 50' 'release 29'
roundtrip recovery-activated
expect_json '([.[] | select(.event == "activated" and .id == "talk")] | length) == 1 and
  ([.[] | select(.event == "deactivated" and .id == "talk")] | length) == 1 and all(.[]; .event != "error")'
finish_client

# Unrelated peers cannot control live sessions or cancel a pending review.
start_client ownership
client_ready
send '{"op":"create","tag":"owner-create"}'
response owner-create 0
send '{"op":"bind","tag":"owner-bind","shortcuts":[{"id":"talk","description":"Talk \"quoted\"\nnext line","preferred_trigger":"CTRL+m"}]}'
response owner-bind 0
expect_restored_snapshot owner-bind ownership
send '{"op":"create","session":"pending","tag":"owner-pending-create"}'
response owner-pending-create 0
send '{"op":"bind","session":"pending","tag":"owner-pending-bind","shortcuts":[{"id":"owner-pending","description":"Pending","preferred_trigger":"CTRL+n"}]}'
review true
owner_session=$(jq -sr '[.[] | select(.event == "response" and .tag == "owner-create")][0].session_handle' "$CLIENT_LOG")
pending_request=$(jq -sr '[.[] | select(.event == "requested" and .tag == "owner-pending-bind")][0].request_handle' "$CLIENT_LOG")
"$PORTAL_BUILD/global-shortcuts-test-client" --probe-session "$owner_session" "$pending_request" \
  > "$RUNTIME/ownership-peer.jsonl"
jq -se 'length == 4 and all(.[]; .event == "rejected" and .replacement == false) and
  ([.[].method] | sort) == ["BindShortcuts", "ListShortcuts", "Request.Close", "Session.Close"]' \
  "$RUNTIME/ownership-peer.jsonl" > /dev/null
review true
roundtrip owner-preserved
expect_restored_snapshot owner-preserved ownership

# A crashed frontend cannot clean up for the backend; owner loss must retire both sessions.
kill -KILL "$frontend_pid"
frontend_status=0
wait "$frontend_pid" 2>/dev/null || frontend_status=$?
[[ $frontend_status == 137 ]]
forget_pid "$frontend_pid"
wait_owner org.freedesktop.portal.Desktop false
no_live_sessions
review false
"$PORTAL_BUILD/global-shortcuts-test-client" --probe-replacement "$owner_session" \
  > "$RUNTIME/ownership-replacement.jsonl"
jq -se 'length == 1 and .[0] == {"event":"rejected","method":"ListShortcuts","replacement":true}' \
  "$RUNTIME/ownership-replacement.jsonl" > /dev/null
wait_owner org.freedesktop.portal.Desktop false
finish_client
start_frontend owner-recovery
restore_after_restart owner-recovery

# Approved sessions of one application compete in successful-bind order.
start_client concurrent
client_ready
send '{"op":"create","tag":"leader-create"}'
response leader-create 0
send '{"op":"bind","tag":"leader-bind","shortcuts":[{"id":"talk","description":"Talk \"quoted\"\nnext line","preferred_trigger":"CTRL+m"}]}'
response leader-bind 0
expect_restored_snapshot leader-bind concurrent
for waiter in first second; do
  send "{\"op\":\"create\",\"session\":\"$waiter\",\"tag\":\"$waiter-create\"}"
  response "$waiter-create" 0
  send "{\"op\":\"bind\",\"session\":\"$waiter\",\"tag\":\"$waiter-bind\",\"shortcuts\":[{\"id\":\"talk\",\"description\":\"Waiting\",\"preferred_trigger\":\"CTRL+m\"}]}"
  response "$waiter-bind" 0
  expect_json "any(.[]; .event == \"response\" and .tag == \"$waiter-bind\" and .shortcuts == [])"
  roundtrip "$waiter-list" "$waiter"
  expect_json "any(.[]; .event == \"response\" and .tag == \"$waiter-list\" and .shortcuts == [])"
done
review false
physical 'press 29' 'press 50'
await_json 'any(.[]; .event == "activated" and .session == "main" and .id == "talk")' "original owner activation"
send '{"op":"close"}'
await_json 'any(.[]; .event == "changed" and .session == "first" and .shortcuts ==
  [{"id":"talk","description":"Waiting","trigger_description":"Ctrl+M"}])' "first waiter promotion"
physical 'release 50' 'release 29'
roundtrip first-promoted first
expect_json '([.[] | select(.event == "activated")] | length) == 1 and
  all(.[]; .event != "changed" or .session == "first") and
  any(.[]; .event == "response" and .tag == "first-promoted" and .shortcuts ==
    [{"id":"talk","description":"Waiting","trigger_description":"Ctrl+M"}])'
physical 'press 29' 'tap 50' 'release 29'
roundtrip first-activated first
expect_json '([.[] | select(.event == "activated" and .session == "first")] | length) == 1 and
  ([.[] | select(.event == "deactivated" and .session == "first")] | length) == 1'
send '{"op":"close","session":"first"}'
await_json 'any(.[]; .event == "changed" and .session == "second" and .shortcuts ==
  [{"id":"talk","description":"Waiting","trigger_description":"Ctrl+M"}])' "second waiter promotion"
physical 'press 29' 'tap 50' 'release 29'
roundtrip second-activated second
expect_json '([.[] | select(.event == "activated")] | length) == 3 and
  ([.[] | select(.event == "activated" and .session == "second")] | length) == 1 and
  ([.[] | select(.event == "deactivated" and .session == "second")] | length) == 1 and
  all(.[]; .event != "error") and
  any(.[]; .event == "response" and .tag == "second-activated" and .shortcuts ==
    [{"id":"talk","description":"Waiting","trigger_description":"Ctrl+M"}])'
finish_client

# Revocation retires held input and rejects a cached credential from before it.
start_client revocation
client_ready
send '{"op":"create","tag":"revoke-create"}'
response revoke-create 0
send '{"op":"bind","tag":"revoke-bind","shortcuts":[{"id":"talk","description":"Talk \"quoted\"\nnext line","preferred_trigger":"CTRL+m"}]}'
response revoke-bind 0
expect_restored_snapshot revoke-bind revocation
physical 'press 29' 'press 50'
await_json 'any(.[]; .event == "activated" and .session == "main" and .id == "talk")' "activation before revocation"
cp "$CREDENTIAL_CACHE" "$RUNTIME/cache-before-revoke.json"
"$UMBRIEL" global-shortcuts-revoke "$APPLICATION" > /dev/null
await_json 'any(.[]; .event == "deactivated" and .session == "main" and .id == "talk")' "held activation retired by revoke"
await_json 'any(.[]; .event == "closed" and .session == "main")' "revoked owner closure"
no_live_sessions
physical 'release 50' 'release 29' 'mark revoked-release'
await_events "$COMPOSITOR_LOG" 'mark revoked-release$' 1 "release after revocation"

cp "$RUNTIME/cache-before-revoke.json" "$CREDENTIAL_CACHE"
send '{"op":"create","session":"retry","tag":"revoked-restore"}'
response revoked-restore 0
roundtrip revoked-list retry
expect_json 'any(.[]; .event == "response" and .tag == "revoked-list" and .shortcuts == [])'
if [[ -e $CREDENTIAL_CACHE ]] && ! jq -e --arg app "$APPLICATION" \
  'all(.credentials[]; .application != $app)' "$CREDENTIAL_CACHE" > /dev/null; then
  echo "revoked credential remained in backend cache" >&2
  exit 1
fi

# Revoking an unapproved retry cancels its review and leaves no reusable grant.
send '{"op":"bind","session":"retry","tag":"revoked-retry","shortcuts":[{"id":"talk","description":"Talk \"quoted\"\nnext line","preferred_trigger":"CTRL+m"}]}'
review true
"$UMBRIEL" global-shortcuts-revoke "$APPLICATION" > /dev/null
response revoked-retry 2
await_json 'any(.[]; .event == "closed" and .session == "retry")' "revoked pending review closure"
no_live_sessions
review false
send '{"op":"create","session":"reapproved","tag":"reapproved-create"}'
response reapproved-create 0
send '{"op":"bind","session":"reapproved","tag":"reapproved-bind","shortcuts":[{"id":"talk","description":"Talk \"quoted\"\nnext line","preferred_trigger":"CTRL+m"}]}'
review true
physical 'tap 28'
response reapproved-bind 0
expect_restored_snapshot reapproved-bind revocation
[[ -s $CREDENTIAL_CACHE ]]
physical 'press 29' 'tap 50' 'release 29'
roundtrip reapproved-activated reapproved
expect_json '([.[] | select(.event == "activated")] | length) == 2 and
  ([.[] | select(.event == "deactivated")] | length) == 2 and
  ([.[] | select(.event == "activated" and .session == "reapproved" and .id == "talk")] | length) == 1 and
  ([.[] | select(.event == "deactivated" and .session == "reapproved" and .id == "talk")] | length) == 1 and
  all(.[]; .event != "error")'
finish_client
SUCCESS=1
echo "public global shortcuts approval, routing, snapshots, persistence, failed saves, ownership, promotion, and revocation passed"
