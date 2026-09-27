#!/usr/bin/env bash
# Unlocking restores focus where the lock found it. An active workspace with no window keeps no focused window, so the
# unlock must not reach for the most recently focused window elsewhere: that would drag the compositor onto another
# workspace behind the lock screen. The check observes the active workspace rather than the window list, because the
# workspace switch is what the user sees when the lock screen disappears.
set -euo pipefail

readonly WINDOW_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOCK_CLIENT="${UMBRIEL_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly LOCK_LOG="$UMBRIEL_RUNTIME_DIR/lock-client.log"
readonly LOCK_FIFO="$UMBRIEL_RUNTIME_DIR/lock-control"

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name'
}

"$WINDOW_CLIENT" lock-focus-window > "$UMBRIEL_RUNTIME_DIR/lock-focus-window.log" 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.05
done
if [[ $("$UMBRIEL" windows --json | jq 'length') -ne 1 ]]; then
  echo "the window never mapped: $("$UMBRIEL" windows --json)"
  exit 1
fi

"$UMBRIEL" msg workspace-switch:2 > /dev/null
for _ in $(seq 40); do
  [[ $(active_workspace) == 2 ]] && break
  sleep 0.05
done
if [[ $(active_workspace) != 2 ]]; then
  echo "the empty workspace never became active: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

mkfifo "$LOCK_FIFO"
exec {lock_fd}<> "$LOCK_FIFO"
"$LOCK_CLIENT" <&"$lock_fd" > "$LOCK_LOG" 2>&1 &
for _ in $(seq 100); do
  grep -q '^locked$' "$LOCK_LOG" && break
  sleep 0.05
done
if ! grep -q '^locked$' "$LOCK_LOG"; then
  echo "the session never locked: $(cat "$LOCK_LOG")"
  exit 1
fi

echo unlock >&"$lock_fd"
for _ in $(seq 100); do
  grep -q '^unlocked$' "$LOCK_LOG" && break
  sleep 0.05
done
if ! grep -q '^unlocked$' "$LOCK_LOG"; then
  echo "the session never unlocked: $(cat "$LOCK_LOG")"
  exit 1
fi

# Focus lands synchronously in the unlock handler, so anything the check reads
# after "unlocked" is the final state.
after=$(active_workspace)
if [[ $after != 2 ]]; then
  echo "unlocking left workspace '$after' active instead of the empty workspace 2"
  exit 1
fi
active_windows=$("$UMBRIEL" windows --json | jq '[.[] | select(.active)] | length')
if [[ $active_windows -ne 0 ]]; then
  echo "unlocking activated a window on another workspace: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "unlocking kept the empty workspace active with no activated window"
