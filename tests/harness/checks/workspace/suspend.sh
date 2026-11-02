#!/usr/bin/env bash
# A window on an inactive workspace is told it is suspended and gets no frame callbacks, so a client such as Firefox
# detects the occlusion instead of measuring a slow keepalive as dropped frames. The overview draws it live, so it
# resumes while the overview is open, and coming back to its workspace resumes it for good. A client too old to be
# told it is suspended keeps the slow keepalive, so a loop gated on frame callbacks does not stall.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/hidden.log"
readonly LEGACY_LOG="$UMBRIEL_RUNTIME_DIR/legacy.log"
readonly FIFO="$UMBRIEL_RUNTIME_DIR/hidden.fifo"
readonly LEGACY_FIFO="$UMBRIEL_RUNTIME_DIR/legacy.fifo"

mkfifo "$FIFO" "$LEGACY_FIFO"
# LOG_SUSPENDED binds xdg-shell version 6; without it the client binds version 1, which has no suspended state.
LOG_SUSPENDED=1 REPAINT_ON_STDIN=1 "$CLIENT" hidden 400 300 < "$FIFO" > "$LOG" 2>&1 &
exec 3> "$FIFO"
REPAINT_ON_STDIN=1 "$CLIENT" legacy 400 300 < "$LEGACY_FIFO" > "$LEGACY_LOG" 2>&1 &
exec 4> "$LEGACY_FIFO"

for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') == 2 ]] && break
  sleep 0.1
done
"$UMBRIEL" settle

# The last suspended-state line the client printed, or "resumed" before it printed any.
last_state() {
  local state
  state=$(grep -E '^(suspended|resumed)$' "$LOG" | tail -n 1) || true
  echo "${state:-resumed}"
}

transitions() {
  grep -cE '^(suspended|resumed)$' "$LOG" || true
}

repaints() {
  grep -c '^repainted ' "$1" || true
}

# Waits for the client to report `expected` as a new transition, so a state it reported earlier cannot satisfy it.
wait_for_transition() {
  local expected=$1 before=$2
  for _ in $(seq 50); do
    if (($(transitions) > before)) && [[ $(last_state) == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected the window to become $expected after $3, got $(last_state): $(< "$LOG")"
  exit 1
}

wait_for_repaint() {
  local log=$1 before=$2
  for _ in $(seq 50); do
    (($(repaints "$log") > before)) && return 0
    sleep 0.1
  done
  echo "$3 never received its frame callback: $(< "$log")"
  exit 1
}

# A tiled opener waits hidden for the arrange that places it; that wait must not reach the client as a suspend.
if (($(transitions) != 0)); then
  echo "a window opening on the visible workspace was suspended: $(< "$LOG")"
  exit 1
fi

count=$(transitions)
"$UMBRIEL" msg workspace-switch 2 > /dev/null
"$UMBRIEL" settle
wait_for_transition suspended "$count" "leaving its workspace"

# Both hidden clients ask for a frame callback. Only the one that cannot be suspended gets the keepalive.
hidden_repaints=$(repaints "$LOG")
legacy_repaints=$(repaints "$LEGACY_LOG")
printf R >&3
printf R >&4
wait_for_repaint "$LEGACY_LOG" "$legacy_repaints" "the hidden pre-v6 client"
sleep 0.5 # real time: several keepalive intervals pass without a callback for the suspended client
if (($(repaints "$LOG") > hidden_repaints)); then
  echo "the suspended client received a keepalive frame callback: $(< "$LOG")"
  exit 1
fi

count=$(transitions)
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
wait_for_transition resumed "$count" "the overview drew its card"
# The card's buffer paces the client, so its pending callback is answered.
wait_for_repaint "$LOG" "$hidden_repaints" "the client the overview drew"

count=$(transitions)
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle
wait_for_transition suspended "$count" "the overview closed"

count=$(transitions)
"$UMBRIEL" msg workspace-switch 1 > /dev/null
"$UMBRIEL" settle
wait_for_transition resumed "$count" "returning to its workspace"

echo "a hidden window was suspended without keepalive callbacks, resumed for the overview, and resumed on return," \
  "while a client too old for the suspended state kept its keepalive"
