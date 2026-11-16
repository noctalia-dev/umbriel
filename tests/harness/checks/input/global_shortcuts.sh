#!/usr/bin/env bash
# Restored shortcut sessions own balanced key events through routing gates and lifecycle changes.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT=$UMBRIEL_GLOBAL_SHORTCUTS_CLIENT
readonly LOG=$UMBRIEL_RUNTIME_DIR/shortcuts.log
readonly LOCAL=$UMBRIEL_RUNTIME_DIR/local.log
readonly TOKEN=$UMBRIEL_RUNTIME_DIR/grant
readonly OTHER_TOKEN=$UMBRIEL_RUNTIME_DIR/other-grant
readonly BINDS=$UMBRIEL_RUNTIME_DIR/binds.toml
: > "$BINDS"
cat >> "$UMBRIEL_CONFIG" <<EOF

[include]
files = ["$BINDS"]
[keybinds]
"Ctrl+N" = "workspace-set-layout master"
"Mod+F10" = "submap test"
"submap[test],Escape" = "submap reset"
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

# A claimed application name cannot inherit the fixture's durable approval.
send 'new org.test.App' 'add talk CTRL+m' bind
sync_bridge unapproved
expect_count "$LOG" 'bound session=0 .*status=2' 1
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

echo "global shortcuts preserve key ownership through routing gates, reloads, promotion, and teardown"
