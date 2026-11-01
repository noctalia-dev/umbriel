#!/usr/bin/env bash
# Manual passthrough follows the exact focused surface without a client request.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly KEY_1=2
readonly KEY_2=3
readonly KEY_MARKER=4
readonly KEY_ESC=1
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly LOCK_CLIENT="${UMBRIEL_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/shortcuts-manual.log"
readonly OTHER_LOG="$UMBRIEL_RUNTIME_DIR/shortcuts-other.log"
readonly CONTROL="$UMBRIEL_RUNTIME_DIR/shortcuts-control"
readonly LOCK_LOG="$UMBRIEL_RUNTIME_DIR/shortcuts-lock.log"
readonly LOCK_CONTROL="$UMBRIEL_RUNTIME_DIR/shortcuts-lock-control"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[keybinds]
"Mod+1" = "workspace-set-layout master"
"Mod+2" = { action = "workspace-set-layout dwindle", allow_when_inhibited = true }
"Mod+Escape" = { action = "shortcuts-inhibit-toggle", allow_when_inhibited = true, repeat = false }
EOF
"$UMBRIEL" msg config-reload > /dev/null

focused_layout() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.focused) | .layout'
}

wait_for() {
  local description=$1 command=$2
  for _ in $(seq 60); do
    eval "$command" && return 0
    sleep 0.1
  done
  echo "timed out waiting for $description"
  return 1
}

# A final unbound key confirms that all earlier input reached the client loop.
input() {
  local log=$1
  shift
  local expected
  expected=$(($(events "$log" "keyboard-key code=$KEY_MARKER state=released") + 1))
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" keyboard-only "$@" tap "$KEY_MARKER"
  await_events "$log" "keyboard-key code=$KEY_MARKER state=released" "$expected"
}

assert_keys() {
  local log=$1 key=$2 expected=$3 presses releases
  presses=$(events "$log" "keyboard-key code=$key state=pressed")
  releases=$(events "$log" "keyboard-key code=$key state=released")
  if ((presses != expected || releases != expected)); then
    echo "expected $expected balanced events for key $key, got $presses presses and $releases releases"
    echo "log: $(tr '\n' '|' < "$log")"
    exit 1
  fi
}

assert_layout() {
  local expected=$1
  if [[ $(focused_layout) != "$expected" ]]; then
    echo "expected layout $expected, got $(focused_layout)"
    exit 1
  fi
}

control() {
  local command=$1 expected
  expected=$(($(events "$CLIENT_LOG" "shortcuts-command $command") + 1))
  printf '%s' "$command" >&"$control_fd"
  await_events "$CLIENT_LOG" "shortcuts-command $command" "$expected"
}

if "$UMBRIEL" msg shortcuts-inhibit-toggle > "$UMBRIEL_RUNTIME_DIR/no-focus.log" 2>&1; then
  echo "manual passthrough succeeded without a focused surface"
  exit 1
fi

mkfifo "$CONTROL"
SHORTCUTS_INHIBIT_CONTROL=1 "$OBSERVER" shortcuts-manual < "$CONTROL" > "$CLIENT_LOG" 2>&1 &
manual_pid=$!
exec {control_fd}> "$CONTROL"
wait_for "the manual test window" '[[ $("$UMBRIEL" windows --json | jq length) == 1 ]]'
manual_id=$("$UMBRIEL" windows --json | jq -r '.[0].id')

# The IPC action succeeds without a protocol object and forwards both halves.
"$UMBRIEL" msg shortcuts-inhibit-toggle > /dev/null
input "$CLIENT_LOG" mod logo tap "$KEY_1" mod none
assert_layout scrolling
assert_keys "$CLIENT_LOG" "$KEY_1" 1

# Explicit exemptions, including the exit binding, remain compositor-owned.
input "$CLIENT_LOG" mod logo tap "$KEY_2" mod none
assert_layout dwindle
assert_keys "$CLIENT_LOG" "$KEY_2" 0
input "$CLIENT_LOG" mod logo tap "$KEY_ESC" mod none
"$UMBRIEL" msg workspace-set-layout scrolling > /dev/null
input "$CLIENT_LOG" mod logo tap "$KEY_1" mod none
assert_layout master
assert_keys "$CLIENT_LOG" "$KEY_1" 1
assert_keys "$CLIENT_LOG" "$KEY_ESC" 0

# Key ownership remains balanced when inhibition changes during a held key.
"$UMBRIEL" msg workspace-set-layout scrolling > /dev/null
input "$CLIENT_LOG" mod logo key-press "$KEY_1" tap "$KEY_ESC" key-release "$KEY_1" mod none
assert_layout master
assert_keys "$CLIENT_LOG" "$KEY_1" 1
"$UMBRIEL" msg workspace-set-layout scrolling > /dev/null
input "$CLIENT_LOG" mod logo key-press "$KEY_1" tap "$KEY_ESC" key-release "$KEY_1" mod none
assert_layout scrolling
assert_keys "$CLIENT_LOG" "$KEY_1" 2
assert_keys "$CLIENT_LOG" "$KEY_ESC" 0

# Another focused surface uses normal shortcuts; returning restores passthrough.
input "$CLIENT_LOG" mod logo tap "$KEY_ESC" mod none
"$OBSERVER" shortcuts-other > "$OTHER_LOG" 2>&1 &
wait_for "the other test window" '[[ $("$UMBRIEL" windows --json | jq length) == 2 ]]'
wait_for "the other surface to gain focus" \
  '[[ $("$UMBRIEL" windows --json | jq -r '\''.[] | select(.focused) | .title'\'') == shortcuts-other ]]'
"$UMBRIEL" msg workspace-set-layout scrolling > /dev/null
input "$OTHER_LOG" mod logo tap "$KEY_1" mod none
assert_layout master
assert_keys "$OTHER_LOG" "$KEY_1" 0
"$UMBRIEL" msg window-focus "$manual_id" > /dev/null
"$UMBRIEL" msg workspace-set-layout scrolling > /dev/null
input "$CLIENT_LOG" mod logo tap "$KEY_1" mod none
assert_layout scrolling
assert_keys "$CLIENT_LOG" "$KEY_1" 3

# A client may create and destroy an inhibitor after manual passthrough starts.
control i
await_events "$CLIENT_LOG" 'shortcuts-inhibitor active' 1
control u
input "$CLIENT_LOG" mod logo tap "$KEY_1" mod none
assert_layout scrolling
assert_keys "$CLIENT_LOG" "$KEY_1" 4
control i
await_events "$CLIENT_LOG" 'shortcuts-inhibitor active' 2

# One toggle disables both sources, including after the client object disappears.
input "$CLIENT_LOG" mod logo tap "$KEY_ESC" mod none
await_events "$CLIENT_LOG" 'shortcuts-inhibitor inactive' 1
input "$CLIENT_LOG" mod logo tap "$KEY_1" mod none
assert_layout master
assert_keys "$CLIENT_LOG" "$KEY_1" 4
control u
"$UMBRIEL" msg workspace-set-layout scrolling > /dev/null
input "$CLIENT_LOG" mod logo tap "$KEY_1" mod none
assert_layout master
assert_keys "$CLIENT_LOG" "$KEY_1" 4
assert_keys "$CLIENT_LOG" "$KEY_ESC" 0

# Locking rejects changes and preserves passthrough when its surface is refocused.
"$UMBRIEL" msg shortcuts-inhibit-toggle > /dev/null
mkfifo "$LOCK_CONTROL"
exec {lock_fd}<> "$LOCK_CONTROL"
"$LOCK_CLIENT" <&"$lock_fd" > "$LOCK_LOG" 2>&1 &
lock_pid=$!
await_events "$LOCK_LOG" locked 1
if "$UMBRIEL" msg shortcuts-inhibit-toggle > "$UMBRIEL_RUNTIME_DIR/locked-toggle.log" 2>&1; then
  echo "manual passthrough succeeded while the session was locked"
  exit 1
fi
echo unlock >&"$lock_fd"
await_events "$LOCK_LOG" unlocked 1
wait "$lock_pid"
"$UMBRIEL" settle
# Explicitly transfer focus so the assertion observes a fresh keyboard enter.
other_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "shortcuts-other") | .id')
"$UMBRIEL" msg window-focus "$other_id" > /dev/null
expected_enters=$(($(events "$CLIENT_LOG" keyboard-enter) + 1))
"$UMBRIEL" msg window-focus "$manual_id" > /dev/null
await_events "$CLIENT_LOG" keyboard-enter "$expected_enters"
wait_for "the original surface to regain focus" \
  '[[ $("$UMBRIEL" windows --json | jq -r '\''.[] | select(.focused) | .id'\'') == "$manual_id" ]]'
"$UMBRIEL" msg workspace-set-layout scrolling > /dev/null
input "$CLIENT_LOG" mod logo tap "$KEY_1" mod none
assert_layout scrolling
assert_keys "$CLIENT_LOG" "$KEY_1" 5

# Destroying the manually inhibited focused surface restores normal shortcuts.
kill "$manual_pid"
wait_for "the manual window to close" '[[ $("$UMBRIEL" windows --json | jq length) == 1 ]]'
"$UMBRIEL" msg workspace-set-layout scrolling > /dev/null
input "$OTHER_LOG" mod logo tap "$KEY_1" mod none
assert_layout master
assert_keys "$OTHER_LOG" "$KEY_1" 0

echo "manual shortcut passthrough follows focus and preserves balanced keys and escape bindings"
