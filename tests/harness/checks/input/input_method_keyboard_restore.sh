#!/usr/bin/env bash
# Destroy a transient IME keyboard while Super remains held on the active
# keyboard and a sibling keyboard is idle. The active source must be restored.
# harness: keyboard=none
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly KEY_1=2
readonly KEY_2=3
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly INPUT_METHOD="${UMBRIEL_INPUT_METHOD_CLIENT:-./build-debug/tests/input-method-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly TEXT_LOG="$UMBRIEL_RUNTIME_DIR/text-input-window.log"
readonly PLAIN_LOG="$UMBRIEL_RUNTIME_DIR/plain-window.log"
readonly INPUT_METHOD_LOG="$UMBRIEL_RUNTIME_DIR/input-method-lifecycle.log"
readonly DRIVER_LOG="$UMBRIEL_RUNTIME_DIR/input-method-driver.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"

[keybinds]
"Mod+1" = "workspace-switch 1"
"Mod+2" = "workspace-switch 2"
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_count() {
  local want=$1
  for _ in $(seq 80); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.05
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_active() {
  local title=$1
  for _ in $(seq 80); do
    [[ $("$UMBRIEL" windows --json | jq -r '[.[] | select(.active) | .title] | first // "none"') == "$title" ]] && return 0
    sleep 0.05
  done
  echo "expected '$title' active, got: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_log_count() {
  local file=$1 pattern=$2 want=$3
  for _ in $(seq 80); do
    [[ $(grep -c "$pattern" "$file" 2>/dev/null || true) -ge $want ]] && return 0
    sleep 0.05
  done
  echo "expected $want '$pattern' lines in $file: $(tr '\n' '|' < "$file" 2>/dev/null || true)"
  return 1
}

wait_for_stable_log() {
  local file=$1 previous=-1 stable=0 current
  for _ in $(seq 80); do
    current=$(wc -l < "$file")
    if [[ $current -eq $previous ]]; then
      stable=$((stable + 1))
      ((stable >= 4)) && return 0
    else
      stable=0
      previous=$current
    fi
    sleep 0.05
  done
  return 1
}

coproc IDLE_DRIVER {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" keyboard-only mod none mark idle-keyboard-ready hold > "$UMBRIEL_RUNTIME_DIR/idle-keyboard.log" 2>&1
}
readonly IDLE_DRIVER_PID
readonly IDLE_DRIVER_INPUT=${IDLE_DRIVER[1]}
wait_for_log_count "$UMBRIEL_RUNTIME_DIR/idle-keyboard.log" '^idle-keyboard-ready$' 1

ENABLE_TEXT_INPUT=1 LOG_MODIFIERS=1 "$OBSERVER" text-input-window > "$TEXT_LOG" 2>&1 &
wait_for_count 1

coproc DRIVER {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" keyboard-only \
    mark keyboard-ready hold \
    mod logo tap "$KEY_2" mod none mark first-switch hold \
    mod logo tap "$KEY_1" mod none mark return-to-text hold \
    key-press 125 mod logo tap "$KEY_2" mark final-switch hold \
    mod none key-release 125 mark physical-super-released hold > "$DRIVER_LOG" 2>&1
}
readonly DRIVER_PID
readonly DRIVER_INPUT=${DRIVER[1]}
wait_for_log_count "$DRIVER_LOG" '^keyboard-ready$' 1

"$INPUT_METHOD" activation-lifecycle > "$INPUT_METHOD_LOG" 2>&1 &
wait_for_log_count "$TEXT_LOG" '^text-input-enter$' 1
wait_for_log_count "$INPUT_METHOD_LOG" '^activated$' 1
wait_for_log_count "$INPUT_METHOD_LOG" '^grabbed$' 1

printf '\n' >&"$DRIVER_INPUT"
wait_for_log_count "$DRIVER_LOG" '^first-switch$' 1
wait_for_log_count "$INPUT_METHOD_LOG" '^deactivated$' 1

LOG_MODIFIERS=1 "$OBSERVER" plain-window > "$PLAIN_LOG" 2>&1 &
wait_for_count 2
wait_for_active plain-window

printf '\n' >&"$DRIVER_INPUT"
wait_for_log_count "$DRIVER_LOG" '^return-to-text$' 1
wait_for_active text-input-window
wait_for_log_count "$INPUT_METHOD_LOG" '^activated$' 2
wait_for_log_count "$INPUT_METHOD_LOG" '^grabbed$' 2

printf '\n' >&"$DRIVER_INPUT"
wait_for_log_count "$DRIVER_LOG" '^final-switch$' 1
wait_for_active plain-window
wait_for_log_count "$INPUT_METHOD_LOG" '^deactivated$' 2
wait_for_log_count "$PLAIN_LOG" '^keyboard-key code=125 state=released$' 1
wait_for_stable_log "$PLAIN_LOG"
if ! awk '/^keyboard-modifiers/ { last = $0 } END { exit(last ~ /depressed=64 / ? 0 : 1) }' "$PLAIN_LOG"; then
  echo "IME teardown cleared Super while the active keyboard still held it: $(tr '\n' '|' < "$PLAIN_LOG")"
  exit 1
fi

printf '\n' >&"$DRIVER_INPUT"
wait_for_log_count "$DRIVER_LOG" '^physical-super-released$' 1
wait_for_log_count "$PLAIN_LOG" '^keyboard-key code=125 state=released$' 2
wait_for_stable_log "$PLAIN_LOG"
if ! awk '/^keyboard-modifiers/ { last = $0 } END { exit(last ~ /depressed=0 / ? 0 : 1) }' "$PLAIN_LOG"; then
  echo "physical Super release did not clear the restored keyboard mask"
  exit 1
fi

printf '\n' >&"$DRIVER_INPUT"
wait "$DRIVER_PID"

printf '\n' >&"$IDLE_DRIVER_INPUT"
wait "$IDLE_DRIVER_PID"
echo "IME teardown restores the active keyboard until its physical modifier release"
