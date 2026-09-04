#!/usr/bin/env bash
# harness: outputs=2
# The pointer and keyboard focus can sit on different outputs, because `window-focus` moves focus without warping the
# cursor. Unlocking has to restore the output that owned focus when the lock started, not the one under the cursor:
# resolving it from the cursor would activate the other monitor's window while the user was away. Output names are read
# from where the windows actually land, so the check does not depend on the headless layout order.
set -euo pipefail

readonly WINDOW_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOCK_CLIENT="${UMBRIEL_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly LOCK_LOG="$UMBRIEL_RUNTIME_DIR/lock-client.log"
readonly LOCK_FIFO="$UMBRIEL_RUNTIME_DIR/lock-control"

windows() { "$UMBRIEL" windows --json; }

wait_for_windows() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.05
  done
  echo "expected $want window(s), got $(windows)"
  return 1
}

# Seat activation, which is global, unlike the per-workspace `focused` flag that
# both windows carry here.
active_title() { windows | jq -r '[.[] | select(.active) | .title] | join(",")'; }

window_output() {
  local workspace
  workspace=$(windows | jq -r --arg title "$1" '.[] | select(.title == $title) | .workspace')
  echo "${workspace%%:*}"
}

# The workspace the compositor reports as focused is the active one on the
# pointer's output, which is how this check observes where the cursor sits.
cursor_output() { "$UMBRIEL" workspaces --json | jq -r '.[] | select(.focused) | .output'; }

"$WINDOW_CLIENT" lock-focus-stay > "$UMBRIEL_RUNTIME_DIR/lock-focus-stay.log" 2>&1 &
wait_for_windows 1
stay_output=$(window_output lock-focus-stay)

"$UMBRIEL" msg output-focus-right > /dev/null
sleep 0.2
"$WINDOW_CLIENT" lock-focus-target > "$UMBRIEL_RUNTIME_DIR/lock-focus-target.log" 2>&1 &
wait_for_windows 2
target_output=$(window_output lock-focus-target)
if [[ -z $stay_output || $stay_output == "$target_output" ]]; then
  echo "the two windows share output '$stay_output': $(windows)"
  exit 1
fi

# Park the cursor back on the first output, then move keyboard focus to the
# second output's window without the pointer following it.
"$UMBRIEL" msg output-focus-left > /dev/null
sleep 0.2
target_id=$(windows | jq -r '.[] | select(.title == "lock-focus-target") | .id')
"$UMBRIEL" msg "window-focus:$target_id" > /dev/null
for _ in $(seq 40); do
  [[ $(active_title) == lock-focus-target ]] && break
  sleep 0.05
done
if [[ $(active_title) != lock-focus-target ]]; then
  echo "focus never moved to the window on $target_output: $(windows)"
  exit 1
fi
if [[ $(cursor_output) != "$stay_output" ]]; then
  echo "the cursor is not on $stay_output, so focus and pointer are not on different outputs: $("$UMBRIEL" workspaces --json)"
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
if [[ -n $(active_title) ]]; then
  echo "the lock left a window activated: $(windows)"
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

restored=$(active_title)
if [[ $restored != lock-focus-target ]]; then
  echo "unlocking activated '$restored' instead of the window on $target_output, which held focus"
  exit 1
fi

echo "unlocking restored focus on $target_output, not on the cursor's output $stay_output"
