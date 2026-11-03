# Sourced by input_capture.sh after the capture lifecycle and geometry checks.
# All tokens and permission stores remain inside that runner's private runtime.

no_confirmation() {
  local description=$1 before
  before=$(events "$RUNTIME/compositor.log" status)
  physical status
  await_events "$RUNTIME/compositor.log" status "$((before + 1))" "$description"
  if ! grep '^status ' "$RUNTIME/compositor.log" | tail -1 | grep -q 'confirmation=0'; then
    echo "$description opened an unexpected permission panel"
    exit 1
  fi
}

persistence_client() {
  PERSIST_LOG=$RUNTIME/persist-$1.log
  "$PORTAL_BUILD/input-capture-test-client" <&"$CLIENT_FD" > "$PERSIST_LOG" 2>&1 &
  PERSIST_PID=$!
  PIDS+=("$PERSIST_PID")
  await_events "$PERSIST_LOG" ready 1 "persistence client $1"
}
finish_persistence_client() {
  send close quit
  wait "$PERSIST_PID"
}

# Persistence must be explicitly requested. R cannot turn a mode-zero request
# into a remembered grant, while Enter still permits one capture session.
persistence_client mode-zero
send 'start 3 0'
wait_confirmation
physical 'tap 19'
await_events "$PERSIST_LOG" 'response Start status=1' 1 "mode-zero request cannot be remembered"
assert_count "$PERSIST_LOG" started 0
send new-session 'start 3 0'
wait_confirmation
physical 'tap 28'
await_events "$PERSIST_LOG" 'started capabilities=3 restore=0 clipboard=0' 1 "explicit mode-zero allow once"
send new-session 'start 3 0'
wait_confirmation
assert_count "$PERSIST_LOG" started 1
physical 'tap 1'
await_events "$PERSIST_LOG" 'response Start status=1' 2 "allow once requires a later permission prompt"
send quit
wait "$PERSIST_PID"

# Mode one remains available across sessions of the same application process.
# A different D-Bus peer cannot restore its transient frontend credential.
persistence_client transient
send 'start 3 1'
wait_confirmation
"$BUILD/tests/pointer-client" 1280 720 keyboard-only tap 19
wait_confirmation
assert_count "$PERSIST_LOG" started 0
physical 'tap 19'
await_events "$PERSIST_LOG" 'started capabilities=3 restore=1 clipboard=0' 1 "explicit transient permission"
send "save-token $RUNTIME/transient.token"
await_events "$PERSIST_LOG" restore-token-saved 1 "transient token saved"
send new-session "load-token $RUNTIME/transient.token" 'start 3 1'
await_events "$PERSIST_LOG" 'started capabilities=3 restore=1 clipboard=0' 2 "transient restore in the same peer"
no_confirmation 'same-peer transient restore'
send "save-token $RUNTIME/transient.token"
await_events "$PERSIST_LOG" restore-token-saved 2 "updated transient token saved"
finish_persistence_client
persistence_client transient-other-peer
send "load-token $RUNTIME/transient.token" 'start 3 1'
wait_confirmation
assert_count "$PERSIST_LOG" started 0
physical 'tap 1'
await_events "$PERSIST_LOG" 'response Start status=1' 1 "transient permission expires with the application peer"
send quit
wait "$PERSIST_PID"

for _ in $(seq 200); do
  "$UMBRIEL" input-capture-permissions --json > "$RUNTIME/transient-permissions.json"
  if jq -e 'all(.[]; .persist_mode != 1)' "$RUNTIME/transient-permissions.json" > /dev/null; then break; fi
  sleep 0.025
done
if ! jq -e 'all(.[]; .persist_mode != 1)' "$RUNTIME/transient-permissions.json" > /dev/null; then
  echo 'application disconnect retained an orphaned transient grant'
  exit 1
fi

# Direct bridge probes verify that the compositor checks its own grant even
# when a caller already possesses the frontend's backend-specific credential.
mkfifo "$RUNTIME/persistence-private.fifo"
exec {PERSIST_PRIVATE_FD}<> "$RUNTIME/persistence-private.fifo"
private_send() { printf '%s\n' "$@" >&"$PERSIST_PRIVATE_FD"; }
private_persistence_client() {
  local label=$1 application=$2 token=${3:-}
  PRIVATE_PERSIST_LOG=$RUNTIME/private-persist-$label.log
  CAPTURE_PROTOCOL_VERSION=2 "$BUILD/tests/input-capture-client" <&"$PERSIST_PRIVATE_FD" > "$PRIVATE_PERSIST_LOG" 2>&1 &
  PRIVATE_PERSIST_PID=$!
  PIDS+=("$PRIVATE_PERSIST_PID")
  await_events "$PRIVATE_PERSIST_LOG" ready 1 "private persistence client $label"
  private_send "application $application"
  if [[ -n $token ]]; then private_send "load-grant $token"; fi
}
finish_private_persistence_client() {
  private_send close quit
  wait "$PRIVATE_PERSIST_PID"
}
reject_private_restore() {
  local label=$1 application=$2 token=$3 capabilities=$4
  private_persistence_client "$label" "$application" "$token"
  private_send "start $capabilities 2"
  wait_confirmation
  assert_count "$PRIVATE_PERSIST_LOG" 'started .*status=0' 0
  physical 'tap 1'
  await_events "$PRIVATE_PERSIST_LOG" 'started .*status=1' 1 "untrusted restore requires consent: $label"
  finish_private_persistence_client
}
private_persistence_client named 'org.umbriel.test.receiver'
private_send 'start 2 2'
wait_confirmation
physical 'tap 19'
await_events "$PRIVATE_PERSIST_LOG" 'grant .*mode=2 token=1' 1 "remembered grant for a generic application"
await_events "$PRIVATE_PERSIST_LOG" 'started .*status=0 capabilities=2' 1 "remembered pointer permission started"
private_send "save-grant $RUNTIME/grant-original.token"
await_events "$PRIVATE_PERSIST_LOG" grant-saved 1 "original compositor grant saved"
finish_private_persistence_client
private_persistence_client rotated 'org.umbriel.test.receiver' "$RUNTIME/grant-original.token"
private_send 'start 2 2'
await_events "$PRIVATE_PERSIST_LOG" 'started .*status=0 capabilities=2' 1 "generic application restored"
no_confirmation 'generic application grant restore'
private_send "save-grant $RUNTIME/grant-current.token"
await_events "$PRIVATE_PERSIST_LOG" grant-saved 1 "rotated compositor grant saved"
finish_private_persistence_client
if cmp -s "$RUNTIME/grant-original.token" "$RUNTIME/grant-current.token"; then
  echo 'restoring a compositor grant did not rotate its credential'
  exit 1
fi
reject_private_restore replay 'org.umbriel.test.receiver' "$RUNTIME/grant-original.token" 2
reject_private_restore other-application 'org.umbriel.test.other' "$RUNTIME/grant-current.token" 2
reject_private_restore expanded-capabilities 'org.umbriel.test.receiver' "$RUNTIME/grant-current.token" 3
python3 - "$RUNTIME/grant-current.token" "$RUNTIME/grant-tampered.token" << 'PY'
import pathlib
import sys
value = pathlib.Path(sys.argv[1]).read_text().strip()
pathlib.Path(sys.argv[2]).write_text(('0' if value[0] != '0' else '1') + value[1:] + '\n')
PY
reject_private_restore tampered 'org.umbriel.test.receiver' "$RUNTIME/grant-tampered.token" 2
private_persistence_client unchanged 'org.umbriel.test.receiver' "$RUNTIME/grant-current.token"
private_send 'start 2 2'
await_events "$PRIVATE_PERSIST_LOG" 'started .*status=0 capabilities=2' 1 "rejected attempts do not consume a valid grant"
no_confirmation 'valid grant after rejected restore attempts'
private_send "save-grant $RUNTIME/grant-once.token"
await_events "$PRIVATE_PERSIST_LOG" grant-saved 1 "grant saved before a one-time restoration"
known_id=$("$UMBRIEL" input-capture-permissions --json | jq -er '.[] | select(.application == "org.umbriel.test.receiver") | .id')
finish_private_persistence_client

# Restoring a remembered grant for mode zero consumes its stored credential.
# Revocation by the known permission ID must still stop that active session.
private_persistence_client restored-once 'org.umbriel.test.receiver' "$RUNTIME/grant-once.token"
private_send 'start 2 0'
await_events "$PRIVATE_PERSIST_LOG" 'started .*status=0 capabilities=2' 1 "one-time restoration of a remembered grant"
assert_count "$PRIVATE_PERSIST_LOG" 'grant request=' 0
no_confirmation 'one-time restore from an existing grant'
private_send zones connect
await_events "$PRIVATE_PERSIST_LOG" 'zones .*status=0' 1 "one-time restored zones"
await_events "$PRIVATE_PERSIST_LOG" ei-device-ready 1 "one-time restored pointer"
read -r once_x once_y once_h < <(sed -n 's/^zone request=[0-9]* x=\(-*[0-9]*\) y=\(-*[0-9]*\) width=[0-9]* height=\([0-9]*\)$/\1 \2 \3/p' "$PRIVATE_PERSIST_LOG" | head -1)
private_send "barrier 51 $once_x $once_y $once_x $((once_y + once_h - 1))" enable
await_events "$PRIVATE_PERSIST_LOG" 'barriers .*status=0' 1 "one-time restored exterior barrier"
physical "at $((once_x + 1)) $((once_y + once_h / 2))" 'move -20 0'
await_events "$PRIVATE_PERSIST_LOG" activated 1 "one-time restored session became active"
"$UMBRIEL" input-capture-revoke "$known_id" > "$RUNTIME/revoke-consumed.log"
await_events "$PRIVATE_PERSIST_LOG" closed 1 "known permission ID revokes a consumed grant's active session"
no_confirmation 'revoked one-time restored session'
if ! grep '^status ' "$RUNTIME/compositor.log" | tail -1 | grep -q 'active=0'; then
  echo 'revoking a consumed grant left its active capture running'
  exit 1
fi
finish_private_persistence_client

# A transient bridge grant remains usable within its issuing scope, then
# expires when the frontend reports that the requesting app peer disappeared.
private_persistence_client transient-scope 'org.umbriel.test.transient'
private_send 'start 3 1'
wait_confirmation
physical 'tap 19'
await_events "$PRIVATE_PERSIST_LOG" 'grant .*mode=1 token=1' 1 "transient compositor grant"
private_send "save-grant $RUNTIME/private-transient.token"
await_events "$PRIVATE_PERSIST_LOG" grant-saved 1 "transient compositor token saved"
private_send new-session "load-grant $RUNTIME/private-transient.token" 'start 3 1'
await_events "$PRIVATE_PERSIST_LOG" 'started .*status=0 capabilities=3' 2 "same-scope transient bridge restore"
no_confirmation 'same-scope transient compositor restore'
private_send "save-grant $RUNTIME/private-transient.token"
await_events "$PRIVATE_PERSIST_LOG" grant-saved 2 "rotated transient compositor token saved"
private_send expire-scope new-session "load-grant $RUNTIME/private-transient.token" 'start 3 1'
wait_confirmation
assert_count "$PRIVATE_PERSIST_LOG" 'started .*status=0' 2
physical 'tap 1'
await_events "$PRIVATE_PERSIST_LOG" 'started .*status=1' 1 "expired application scope cannot restore a transient grant"
finish_private_persistence_client

# An empty identifier is a first-class application scope, with explicit
# approval and an opaque credential required just as for named applications.
private_persistence_client blank ''
private_send 'start 3 2'
wait_confirmation
physical 'tap 19'
await_events "$PRIVATE_PERSIST_LOG" 'started .*status=0 capabilities=3' 1 "remembered permission with a blank application identifier"
private_send "save-grant $RUNTIME/grant-blank.token"
await_events "$PRIVATE_PERSIST_LOG" grant-saved 1 "blank-identifier grant saved"
finish_private_persistence_client
private_persistence_client blank-restored '' "$RUNTIME/grant-blank.token"
private_send 'start 3 2'
await_events "$PRIVATE_PERSIST_LOG" 'started .*status=0 capabilities=3' 1 "blank-identifier remembered permission restored"
no_confirmation 'blank application identifier restore'
finish_private_persistence_client

# Public frontend credentials survive a fresh process and compositor restart.
persistence_client durable
send 'start 3 2'
wait_confirmation
physical 'tap 19'
await_events "$PERSIST_LOG" 'started capabilities=3 restore=1 clipboard=0' 1 "explicit persistent permission for a host application"
send "save-token $RUNTIME/durable.token"
await_events "$PERSIST_LOG" restore-token-saved 1 "persistent frontend token saved"
finish_persistence_client
persistence_client durable-fresh-peer
send "load-token $RUNTIME/durable.token" 'start 3 2'
await_events "$PERSIST_LOG" 'started capabilities=3 restore=1 clipboard=0' 1 "persistent restore in a fresh application process"
no_confirmation 'persistent restore in a fresh peer'
send "save-token $RUNTIME/durable.token"
await_events "$PERSIST_LOG" restore-token-saved 1 "persistent token updated before restart"
finish_persistence_client

# Restart only the private compositor and backend, retaining the frontend and
# its permission store. This also destroys every in-memory bridge grant.
kill "$PORTAL_PID"
wait "$PORTAL_PID" 2> /dev/null || true
physical quit
wait "$COMPOSITOR_PID"
cp "$RUNTIME/compositor.log" "$RUNTIME/compositor-before-restart.log"
: > "$RUNTIME/compositor.log"
WAYLAND_DISPLAY=input-capture-isolated-parent WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
  "$COMPOSITOR" "$RUNTIME/config.toml" <&"$PHYSICAL_FD" > "$RUNTIME/compositor.log" 2>&1 &
COMPOSITOR_PID=$!
PIDS+=("$COMPOSITOR_PID")
await_events "$RUNTIME/compositor.log" 'ready display=' 1 "restarted isolated compositor"
"$PORTAL_SERVER" > "$RUNTIME/portal-after-restart.log" 2>&1 &
PORTAL_PID=$!
PIDS+=("$PORTAL_PID")
for _ in $(seq 200); do
  if dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus \
    org.freedesktop.DBus.NameHasOwner string:org.freedesktop.impl.portal.desktop.umbriel 2> /dev/null | grep -q 'boolean true'; then break; fi
  sleep 0.025
done
persistence_client after-restart
send "load-token $RUNTIME/durable.token" 'start 3 2'
await_events "$PERSIST_LOG" 'started capabilities=3 restore=1 clipboard=0' 1 "persistent permission survives compositor restart"
no_confirmation 'persistent restore after compositor restart'
send "save-token $RUNTIME/durable.token" zones connect
await_events "$PERSIST_LOG" restore-token-saved 1 "restored persistent token saved"
await_events "$PERSIST_LOG" 'zone x=' 1 "restored capture zones"
await_events "$PERSIST_LOG" ei-device-ready 2 "restored EI devices"
read -r persistent_x persistent_y persistent_w persistent_h < <(sed -n 's/^zone x=\(-*[0-9]*\) y=\(-*[0-9]*\) width=\([0-9]*\) height=\([0-9]*\)$/\1 \2 \3 \4/p' "$PERSIST_LOG" | head -1)
send "barrier 21 $persistent_x $persistent_y $persistent_x $((persistent_y + persistent_h - 1))" enable
await_events "$PERSIST_LOG" 'barriers failed=0' 1 "remembered capture barrier"
await_events "$PERSIST_LOG" 'requested enable' 1 "remembered capture enabled"
physical "at $((persistent_x + 1)) $((persistent_y + persistent_h / 2))" 'move -20 0'
await_events "$PERSIST_LOG" activated 1 "remembered session captures input"
"$UMBRIEL" input-capture-permissions --json > "$RUNTIME/permissions.json"
python3 - "$RUNTIME/permissions.json" "$XDG_STATE_HOME/umbriel/input-capture-permissions.json" << 'PY'
import pathlib
import sys
listing = pathlib.Path(sys.argv[1]).read_text()
records = pathlib.Path(sys.argv[2]).read_text()
# The inspection command must not print stored bearer credentials.
import json
stored = json.loads(records)
def tokens(value):
    if isinstance(value, dict):
        for key, item in value.items():
            if key in ('token', 'restore_token', 'secret') and isinstance(item, str):
                yield item
            else:
                yield from tokens(item)
    elif isinstance(value, list):
        for item in value:
            yield from tokens(item)
stored_tokens = list(tokens(stored))
assert stored_tokens, 'no durable grants were stored'
assert all(token not in listing for token in stored_tokens), 'permission inspection leaked a restore credential'
PY
"$UMBRIEL" input-capture-revoke all > "$RUNTIME/revoke.log"
for _ in $(seq 200); do
  before=$(events "$RUNTIME/compositor.log" status)
  physical status
  await_events "$RUNTIME/compositor.log" status "$((before + 1))" "revoked active capture status"
  if grep '^status ' "$RUNTIME/compositor.log" | tail -1 | grep -q 'active=0'; then break; fi
  sleep 0.025
done
if ! grep '^status ' "$RUNTIME/compositor.log" | tail -1 | grep -q 'active=0'; then
  echo 'revocation left remembered capture active'
  exit 1
fi
send new-session "load-token $RUNTIME/durable.token" 'start 3 2'
wait_confirmation
assert_count "$PERSIST_LOG" 'started capabilities=3' 1
physical 'tap 1'
await_events "$PERSIST_LOG" 'response Start status=1' 1 "revoked permission requires fresh consent"
send quit
wait "$PERSIST_PID"
