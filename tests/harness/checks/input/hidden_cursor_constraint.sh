#!/usr/bin/env bash
# A cursor hidden while a client holds a pointer lock or confinement must keep
# pointer focus: the constrained surface gets no pointer-leave, the constraint
# stays activated, and the next motion does not churn enter/leave either.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly SEAT_LOG="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly KEY_A=30
readonly GAME=hidden-cursor-constraint
readonly LOG="$UMBRIEL_RUNTIME_DIR/$GAME.log"

window() { "$UMBRIEL" windows --json | jq -c --arg title "$GAME" '.[] | select(.title == $title)'; }
events() { grep -c "^$1" "$LOG" || true; }

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[input.cursor]
hide_when_typing = true
hide_timeout_ms = 0
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$UMBRIEL_RUNTIME_DIR/$GAME.fifo"
exec {GAME_FD}<>"$UMBRIEL_RUNTIME_DIR/$GAME.fifo"
POINTER_CONSTRAINT_CONTROL=1 LOG_POINTER_INPUT=1 "$SEAT_LOG" "$GAME" \
  <&"$GAME_FD" > "$LOG" 2>&1 &
game_pid=$!

for _ in $(seq 60); do
  [[ -n $(window) ]] && break
  sleep 0.1
done
if [[ -z $(window) ]]; then
  echo "$GAME window never appeared: $(cat "$LOG")"
  exit 1
fi

read -r gx gy gw gh < <(window | jq -r '"\(.x) \(.y) \(.w) \(.h)"')
cx=$((gx + gw / 2)); cy=$((gy + gh / 2))
pointer() { "$POINTER" 1280 720 "$@"; }

control() {
  local command=$1 n
  n=$(events "constraint-command $command")
  printf '%s' "$command" >&"$GAME_FD"
  for _ in $(seq 200); do
    [[ $(events "constraint-command $command") -gt $n ]] && return 0
    sleep 0.025
  done
  echo "the client never acknowledged '$command': $(tail -5 "$LOG")"
  return 1
}

# Hover the game so it owns pointer focus, then take a client lock.
pointer move "$cx" "$cy"
"$UMBRIEL" settle
[[ $(events pointer-enter) -ge 1 ]] || { echo "window never received pointer-enter"; exit 1; }

control l
for _ in $(seq 200); do
  grep -q '^pointer-locked$' "$LOG" && break
  sleep 0.025
done
grep -q '^pointer-locked$' "$LOG" || { echo "the lock never activated: $(tail -5 "$LOG")"; exit 1; }

# Move while locked (relative motion still reaches the client), then hide the
# cursor with an unbound key.
pointer relative 5 0
pointer tap "$KEY_A"
"$UMBRIEL" settle
[[ $(events keyboard-key) -ge 1 ]] || { echo "the tap never reached the client"; exit 1; }

if [[ $(events pointer-leave) -ne 0 || $(events pointer-unlocked) -ne 0 ]]; then
  echo "hiding the cursor under a lock dropped pointer focus or the lock: $(tail -10 "$LOG")"
  exit 1
fi

# Motion after the hide must not produce a leave/enter pair either.
pointer relative 3 2
"$UMBRIEL" settle
[[ $(events pointer-leave) -eq 0 ]] || { echo "motion after the hide sent pointer-leave"; exit 1; }
[[ $(events pointer-enter) -eq 1 ]] || { echo "motion after the hide sent a second pointer-enter"; exit 1; }

# Confinement gets the same treatment.
control u
control c
for _ in $(seq 200); do
  grep -q '^pointer-confined$' "$LOG" && break
  sleep 0.025
done
grep -q '^pointer-confined$' "$LOG" || { echo "the confinement never activated: $(tail -5 "$LOG")"; exit 1; }
pointer tap "$KEY_A"
"$UMBRIEL" settle
if [[ $(events pointer-leave) -ne 0 || $(events pointer-unconfined) -ne 0 ]]; then
  echo "hiding the cursor under confinement dropped pointer focus or the confinement: $(tail -10 "$LOG")"
  exit 1
fi

# The exception covers only the constrained surface: a warp onto another window while hidden still hands it nothing.
LOG_POINTER_INPUT=1 "$SEAT_LOG" "$GAME-other" > "$UMBRIEL_RUNTIME_DIR/$GAME-other.log" 2>&1 &
for _ in $(seq 60); do
  other=$("$UMBRIEL" windows --json | jq -r --arg title "$GAME-other" '.[] | select(.title == $title) | .id')
  [[ -n $other ]] && break
  sleep 0.1
done
[[ -n $other ]] || { echo "the second window never appeared"; exit 1; }
"$UMBRIEL" settle
"$UMBRIEL" msg window-focus-warp "$other" > /dev/null
"$UMBRIEL" settle
if grep -qE '^pointer-(enter|motion)' "$UMBRIEL_RUNTIME_DIR/$GAME-other.log"; then
  echo "a warp while hidden under a constraint handed pointer focus to another window"
  exit 1
fi
control u
kill "$game_pid" 2>/dev/null || true

echo "hiding the cursor preserves pointer focus under locks and confinement"
