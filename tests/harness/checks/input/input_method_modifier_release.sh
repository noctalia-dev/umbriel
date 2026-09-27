#!/usr/bin/env bash
# Fcitx can retain its virtual keyboard while creating the keyboard grab only
# when text input is active. The retained keyboard keeps the last grabbed
# modifier mask, so shortcut matching must use the physical keyboard after the
# grab disappears.
# It creates one persistent physical keyboard for the exact device topology.
# harness: keyboard=none
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly KEY_1=2
readonly KEY_2=3
readonly KEY_ENTER=28
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
"Mod+1" = "workspace-switch:1"
"Mod+2" = "workspace-switch:2"
"Mod+Return" = "workspace-switch:1"
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

# Map a text-input-v3 client on workspace 1. The keyboard driver starts with a
# barrier, which creates one persistent physical keyboard before Fcitx connects
# and keeps that same device through the complete reproduction.
ENABLE_TEXT_INPUT=1 LOG_MODIFIERS=1 "$OBSERVER" text-input-window > "$TEXT_LOG" 2>&1 &
wait_for_count 1
coproc DRIVER {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" keyboard-only \
    mark keyboard-ready hold \
    mod logo tap "$KEY_2" mod none mark first-switch hold \
    mod logo tap "$KEY_1" mark return-to-text hold \
    tap "$KEY_2" mark final-switch hold \
    mod none mark modifier-released hold \
    tap "$KEY_ENTER" > "$DRIVER_LOG" 2>&1
}
readonly DRIVER_PID
readonly DRIVER_INPUT=${DRIVER[1]}
wait_for_log_count "$DRIVER_LOG" '^keyboard-ready$' 1

"$INPUT_METHOD" persistent-activation-lifecycle > "$INPUT_METHOD_LOG" 2>&1 &
wait_for_log_count "$TEXT_LOG" '^text-input-enter$' 1
wait_for_log_count "$INPUT_METHOD_LOG" '^activated$' 1
wait_for_log_count "$INPUT_METHOD_LOG" '^grabbed$' 1

# Switch away from active text input, then map a client with no text-input-v3
# object. Fcitx releases its grab but retains its virtual keyboard here.
printf '\n' >&"$DRIVER_INPUT"
wait_for_log_count "$DRIVER_LOG" '^first-switch$' 1
wait_for_log_count "$INPUT_METHOD_LOG" '^deactivated$' 1
"$OBSERVER" plain-window > "$PLAIN_LOG" 2>&1 &
wait_for_count 2
wait_for_active plain-window

# Return to the text input so Fcitx creates a fresh grab for its retained
# virtual keyboard, matching the middle transition in the report.
printf '\n' >&"$DRIVER_INPUT"
wait_for_log_count "$DRIVER_LOG" '^return-to-text$' 1
wait_for_active text-input-window
wait_for_log_count "$INPUT_METHOD_LOG" '^activated$' 2
wait_for_log_count "$INPUT_METHOD_LOG" '^grabbed$' 2

# Keep Mod held while switching back to the plain client, matching the report's
# final Mod+1, Mod+2 sequence. Then release it only after Fcitx has torn down
# the grab while its virtual keyboard still carries the prior Mod mask.
printf '\n' >&"$DRIVER_INPUT"
wait_for_log_count "$DRIVER_LOG" '^final-switch$' 1
wait_for_active plain-window
wait_for_log_count "$INPUT_METHOD_LOG" '^deactivated$' 2
printf '\n' >&"$DRIVER_INPUT"
wait_for_log_count "$DRIVER_LOG" '^modifier-released$' 1
printf '\n' >&"$DRIVER_INPUT"
wait "$DRIVER_PID"

if ! grep -q "keyboard-key code=$KEY_ENTER state=pressed" "$PLAIN_LOG"; then
  echo "plain Enter did not reach the non-text-input window: $(tr '\n' '|' < "$PLAIN_LOG")"
  exit 1
fi
if ! wait_for_active plain-window; then
  echo "plain Enter matched Mod+Return after the Fcitx keyboard handoff"
  exit 1
fi

echo "Fcitx-style text-input handoffs release Mod before the next plain key"
