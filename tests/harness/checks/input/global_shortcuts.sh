#!/usr/bin/env bash
# Restored shortcut sessions own balanced key events through routing gates and lifecycle changes.
set -Eeuo pipefail
trap 'echo "global shortcut check failed at line $LINENO: $BASH_COMMAND" >&2' ERR
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT=$UMBRIEL_GLOBAL_SHORTCUTS_CLIENT
readonly LOG=$UMBRIEL_RUNTIME_DIR/shortcuts.log
readonly LOCAL=$UMBRIEL_RUNTIME_DIR/local.log
readonly TOKEN=$UMBRIEL_RUNTIME_DIR/grant
readonly OTHER_TOKEN=$UMBRIEL_RUNTIME_DIR/other-grant
readonly BINDS=$UMBRIEL_RUNTIME_DIR/binds.toml
readonly REPORT=$UMBRIEL_RUNTIME_DIR/inspection.json
cat >> "$UMBRIEL_CONFIG" <<EOF

[include]
files = ["$BINDS"]
[keybinds]
"Ctrl+N" = "workspace-set-layout master"
"Mod+F10" = "submap test"
"submap[test],Escape" = "submap reset"
EOF
cat > "$BINDS" <<'EOF'
[global_shortcuts."org.test.App"]
talk = "Ctrl+M"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$CLIENT" seed "$TOKEN" org.test.App talk CTRL+m reserved CTRL+n
"$CLIENT" seed "$OTHER_TOKEN" org.test.Other talk CTRL+m
mkfifo "$UMBRIEL_RUNTIME_DIR/control"
exec {control}<> "$UMBRIEL_RUNTIME_DIR/control"
"$CLIENT" <&"$control" > "$LOG" 2>&1 &
await_events "$LOG" ready 1
"$UMBRIEL_SEAT_LOG_CLIENT" shortcut-observer > "$LOCAL" 2>&1 &
await_events "$LOCAL" keyboard-enter 1

send() { printf '%s\n' "$@" >&"$control"; }
sync_bridge() {
  send "sync $1"
  await_events "$LOG" "sync $1$" 1
}
expect_count() {
  local actual
  actual=$(events "$1" "$2")
  if ((actual != $3)); then
    echo "expected $3 '$2', got $actual"
    cat "$1"
    exit 1
  fi
}
keys() {
  local count
  count=$(events "$LOCAL" 'keyboard-key code=30 state=released')
  "$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only "$@" mod none tap 30
  await_events "$LOCAL" 'keyboard-key code=30 state=released' "$((count + 1))"
}
inspect() {
  "$UMBRIEL" global-shortcuts --json > "$REPORT"
  jq -e 'all(.[]; keys == ["application", "live", "shortcuts"] and
    all(.shortcuts[]; keys == ["candidate", "conflict", "description", "id", "pending", "session", "source", "trigger"]))' \
    "$REPORT" > /dev/null
  if grep -F -f "$TOKEN" "$REPORT" > /dev/null || grep -F -f "$OTHER_TOKEN" "$REPORT" > /dev/null; then
    echo "shortcut inspection exposed a restore credential"
    exit 1
  fi
}

inspect
jq -e 'length == 2 and all(.[]; .live == false and all(.shortcuts[]; .session == null and .trigger == null))' \
  "$REPORT" > /dev/null

# A claimed application name cannot inherit the fixture's durable approval.
send 'new org.test.App' 'add talk CTRL+m' bind
sync_bridge unapproved
expect_count "$LOG" 'bound session=0 ' 0
inspect
jq -e 'any(.[]; .application == "org.test.App" and .live and
  any(.shortcuts[]; .pending and .source == "config" and .candidate == "Ctrl+M" and .trigger == null))' \
  "$REPORT" > /dev/null
# A rejected config edit keeps the pending review alive.
cat > "$BINDS" <<'EOF'
[global_shortcuts."org.test.App"]
talk = "Ctrl+MouseLeft"
EOF
"$UMBRIEL" msg config-reload > /dev/null
sync_bridge rejected-review
expect_count "$LOG" 'bound session=0 ' 0
: > "$BINDS"
"$UMBRIEL_POINTER_CLIENT" 1280 720 click 272
sync_bridge cancelled
expect_count "$LOG" 'bound session=0 .*status=1' 1
send close
send 'new org.test.App' "restore $TOKEN" 'add talk CTRL+m' 'add reserved CTRL+n' bind list
sync_bridge restored
expect_count "$LOG" 'restored session=1 .*status=0' 1
expect_count "$LOG" 'bound session=1 .*status=0' 1
expect_count "$LOG" 'shortcut session=1 .*id=reserved' 0

pointer_hold 1280 720 keyboard-only mod control key-press 50 key-press 50 -- key-release 50 mod none tap 30
await_events "$LOG" 'activated session=1 id=talk' 1
sync_bridge repeat
expect_count "$LOG" 'activated session=1 ' 1
pointer_release
await_events "$LOG" 'deactivated session=1 id=talk' 1
sync_bridge released
expect_count "$LOCAL" 'keyboard-key code=50 ' 0
keys mod control tap 49
[[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.focused) | .layout') == master ]]
expect_count "$LOCAL" 'keyboard-key code=49 ' 0

# Inhibition and submaps forward unmatched input without granting shortcut ownership.
"$UMBRIEL" msg shortcuts-inhibit-toggle > /dev/null
keys mod control tap 50
sync_bridge inhibited
expect_count "$LOG" 'activated session=1 ' 1
expect_count "$LOCAL" 'keyboard-key code=50 state=pressed' 1
expect_count "$LOCAL" 'keyboard-key code=50 state=released' 1
"$UMBRIEL" msg shortcuts-inhibit-toggle > /dev/null
"$UMBRIEL" msg submap test > /dev/null
keys mod control tap 50
"$UMBRIEL" msg submap reset > /dev/null
sync_bridge submap
expect_count "$LOG" 'activated session=1 ' 1

# Modal opening retires held activation; its later release stays consumed.
pointer_hold 1280 720 keyboard-only mod control key-press 50 -- key-release 50 mod none tap 30
await_events "$LOG" 'activated session=1 ' 2
"$UMBRIEL" msg session-quit > /dev/null
await_events "$LOG" 'deactivated session=1 ' 2
"$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only tap 1
pointer_release
sync_bridge modal
expect_count "$LOG" 'deactivated session=1 ' 2
expect_count "$LOCAL" 'keyboard-key code=50 state=released' 2

# Keybind reloads publish an empty snapshot and retain consumption of the old key.
pointer_hold 1280 720 keyboard-only mod control key-press 50 -- key-release 50 mod none tap 30
await_events "$LOG" 'activated session=1 ' 3
cat > "$BINDS" <<'EOF'
[keybinds]
"Ctrl+M" = "workspace-set-layout dwindle"
EOF
"$UMBRIEL" msg config-reload > /dev/null
await_events "$LOG" 'deactivated session=1 ' 3
await_events "$LOG" 'changes-done session=1' 1
pointer_release
sync_bridge reserved
expect_count "$LOG" 'changed session=1 ' 0
expect_count "$LOCAL" 'keyboard-key code=50 state=released' 2
: > "$BINDS"
"$UMBRIEL" msg config-reload > /dev/null
await_events "$LOG" 'changed session=1 id=talk' 1

# An approved waiter is promoted when its owner closes, without replaying a held key.
send 'new org.test.Other' "restore $OTHER_TOKEN" 'add talk CTRL+m' bind
sync_bridge waiter
expect_count "$LOG" 'bound session=2 .*status=0' 1
expect_count "$LOG" 'shortcut session=2 ' 0
inspect
jq -e 'any(.[]; .application == "org.test.Other" and .live and
  any(.shortcuts[]; .id == "talk" and .source == "stored" and .conflict == "in_use" and
    .trigger == null and .candidate == "Ctrl+M"))' "$REPORT" > /dev/null
pointer_hold 1280 720 keyboard-only mod control key-press 50 -- key-release 50 mod none tap 30
await_events "$LOG" 'activated session=1 ' 4
send 'select 1' close
await_events "$LOG" 'changed session=2 id=talk' 1
pointer_release
sync_bridge promoted
expect_count "$LOG" 'activated session=2 ' 0
keys mod control tap 50
await_events "$LOG" 'activated session=2 ' 1
await_events "$LOG" 'deactivated session=2 ' 1

# Source destruction retires an activation even when no release was sent.
"$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only mod control key-press 50
await_events "$LOG" 'activated session=2 ' 2
await_events "$LOG" 'deactivated session=2 ' 2

# Lock preparation stops activation before the locker commits its first buffer.
pointer_hold 1280 720 keyboard-only mod control key-press 50 -- key-release 50 mod none
await_events "$LOG" 'activated session=2 ' 3
mkfifo "$UMBRIEL_RUNTIME_DIR/lock-control"
exec {lock_control}<> "$UMBRIEL_RUNTIME_DIR/lock-control"
"$UMBRIEL_LOCK_CLIENT" --defer-commit <&"$lock_control" > "$UMBRIEL_RUNTIME_DIR/lock.log" 2>&1 &
await_events "$UMBRIEL_RUNTIME_DIR/lock.log" configured 1
await_events "$LOG" 'deactivated session=2 ' 3
pointer_release
"$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only mod control tap 50 mod none
sync_bridge preparing
expect_count "$LOG" 'activated session=2 ' 3
printf 'commit\n' >&"$lock_control"
await_events "$UMBRIEL_RUNTIME_DIR/lock.log" locked 1
"$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only mod control tap 50 mod none
sync_bridge locked
expect_count "$LOG" 'activated session=2 ' 3
printf 'unlock\n' >&"$lock_control"
await_events "$UMBRIEL_RUNTIME_DIR/lock.log" unlocked 1
keys mod control tap 50
await_events "$LOG" 'activated session=2 ' 4
await_events "$LOG" 'deactivated session=2 ' 4

# Config overrides rebind live sessions and retire held keys before publishing their replacement.
send 'select 2'
changes=$(events "$LOG" 'changes-done session=2')
releases=$(events "$LOCAL" 'keyboard-key code=50 state=released')
pointer_hold 1280 720 keyboard-only mod control key-press 50 -- key-release 50 mod none tap 30
await_events "$LOG" 'activated session=2 ' 5
cat > "$BINDS" <<'EOF'
[general]
mod_key = "Super"
[global_shortcuts."org.test.Other"]
talk = "Mod+K"
EOF
"$UMBRIEL" msg config-reload > /dev/null
await_events "$LOG" 'deactivated session=2 ' 5
await_events "$LOG" 'changes-done session=2' "$((changes + 1))"
expect_count "$LOG" 'changed session=2 id=talk trigger=Super+K$' 1
pointer_release
sync_bridge override-release
expect_count "$LOCAL" 'keyboard-key code=50 state=released' "$releases"
keys mod control tap 50
expect_count "$LOG" 'activated session=2 ' 5
keys mod super tap 37
await_events "$LOG" 'activated session=2 ' 6
await_events "$LOG" 'deactivated session=2 ' 6

# Symbolic Mod resolves again when the configured modifier changes.
sed -i 's/mod_key = "Super"/mod_key = "Alt"/' "$BINDS"
"$UMBRIEL" msg config-reload > /dev/null
await_events "$LOG" 'changed session=2 id=talk trigger=Alt+K$' 1
keys mod super tap 37
expect_count "$LOG" 'activated session=2 ' 6
keys mod alt tap 37
await_events "$LOG" 'activated session=2 ' 7
await_events "$LOG" 'deactivated session=2 ' 7

# Disabling publishes an empty replacement; removing the override restores the approved candidate.
changes=$(events "$LOG" 'changes-done session=2')
snapshots=$(events "$LOG" 'changed session=2 ')
cat > "$BINDS" <<'EOF'
[global_shortcuts."org.test.Other"]
talk = ""
EOF
"$UMBRIEL" msg config-reload > /dev/null
await_events "$LOG" 'changes-done session=2' "$((changes + 1))"
expect_count "$LOG" 'changed session=2 ' "$snapshots"
inspect
jq -e 'any(.[]; .application == "org.test.Other" and any(.shortcuts[];
  .source == "config" and .conflict == "disabled" and .candidate == null and .trigger == null))' \
  "$REPORT" > /dev/null
keys mod alt tap 37 mod control tap 50
expect_count "$LOG" 'activated session=2 ' 7
: > "$BINDS"
"$UMBRIEL" msg config-reload > /dev/null
await_events "$LOG" 'changed session=2 id=talk trigger=Ctrl+M$' 2
keys mod control tap 50
await_events "$LOG" 'activated session=2 ' 8
await_events "$LOG" 'deactivated session=2 ' 8

# Revocation retires the owner before promoting another application's approved waiter.
send 'new org.test.App' "restore $TOKEN" 'add talk CTRL+m' bind
sync_bridge revoke-waiter
expect_count "$LOG" 'bound session=3 .*status=0' 1
expect_count "$LOG" 'shortcut session=3 ' 0
pointer_hold 1280 720 keyboard-only mod control key-press 50 -- key-release 50 mod none tap 30
await_events "$LOG" 'activated session=2 ' 9
releases=$(events "$LOCAL" 'keyboard-key code=50 state=released')
"$UMBRIEL" global-shortcuts-revoke org.test.Other
await_events "$LOG" 'deactivated session=2 ' 9
await_events "$LOG" 'closed session=2' 1
await_events "$LOG" 'changed session=3 id=talk' 1
inspect
jq -e 'all(.[]; .application != "org.test.Other") and any(.[];
  .application == "org.test.App" and any(.shortcuts[]; .trigger == "Ctrl+M" and .source == "stored"))' \
  "$REPORT" > /dev/null
pointer_release
sync_bridge revoked-release
expect_count "$LOCAL" 'keyboard-key code=50 state=released' "$releases"
expect_count "$LOG" 'activated session=3 ' 0
send 'new org.test.Other' "restore $OTHER_TOKEN"
sync_bridge revoked-credential
expect_count "$LOG" 'restored session=4 .*status=2' 1
send close
keys mod control tap 50
await_events "$LOG" 'activated session=3 ' 1

# A failed durable deletion still closes live sessions, and inspection reports an unreadable store.
pointer_hold 1280 720 keyboard-only mod control key-press 50 -- key-release 50 mod none tap 30
await_events "$LOG" 'activated session=3 ' 2
chmod 0644 "$XDG_STATE_HOME/umbriel/global-shortcuts.json"
if "$UMBRIEL" global-shortcuts-revoke org.test.App > /dev/null 2>&1; then
  echo "shortcut revocation hid a durable deletion failure"
  exit 1
fi
await_events "$LOG" 'deactivated session=3 ' 2
await_events "$LOG" 'closed session=3' 1
if "$UMBRIEL" global-shortcuts --json > /dev/null 2>&1; then
  echo "shortcut inspection hid an unreadable store"
  exit 1
fi
pointer_release
chmod 0600 "$XDG_STATE_HOME/umbriel/global-shortcuts.json"
send 'new org.test.App' "restore $TOKEN" 'add talk CTRL+m' bind
sync_bridge failed-revoke-credential
expect_count "$LOG" 'bound session=5 .*status=0' 1

# Revoke-all cancels a visible review, closes every session, and forgets offline grants.
"$CLIENT" seed "$UMBRIEL_RUNTIME_DIR/offline-grant" org.test.Offline offline F18
send 'new org.test.Pending' "credential $UMBRIEL_RUNTIME_DIR/pending-grant" 'add new F17' bind
sync_bridge pending-revoke
expect_count "$LOG" 'bound session=6 ' 0
"$UMBRIEL" global-shortcuts-revoke all
await_events "$LOG" 'closed session=5' 1
await_events "$LOG" 'closed session=6' 1
expect_count "$LOG" 'bound session=6 .*status=2' 1
expect_count "$LOG" 'grant session=6 ' 0
keys tap 28
inspect
jq -e 'length == 0' "$REPORT" > /dev/null
"$UMBRIEL" global-shortcuts-revoke all
if "$UMBRIEL" global-shortcuts-revoke > /dev/null 2>&1; then
  echo "shortcut revocation accepted a missing selector"
  exit 1
fi

echo "global shortcuts preserve ownership through routing, overrides, inspection, revocation, promotion, and teardown"
