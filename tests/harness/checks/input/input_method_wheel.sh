#!/usr/bin/env bash
# An input method can own a virtual keyboard while grabbing events from physical keyboards. Modifier mouse and wheel
# binds must use the combined physical state, not the input method keyboard that happens to be current on the seat.
# It builds its own keyboard topology, so the harness keyboard stays out.
# harness: keyboard=none
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly INPUT_METHOD="${UMBRIEL_INPUT_METHOD_CLIENT:-./build-debug/tests/input-method-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly INPUT_METHOD_LOG="$UMBRIEL_RUNTIME_DIR/input-method.log"
readonly POINTER_LOG="$UMBRIEL_RUNTIME_DIR/pointer.log"
readonly FIRST_LOG="$UMBRIEL_RUNTIME_DIR/input-first.log"
readonly SECOND_LOG="$UMBRIEL_RUNTIME_DIR/input-second.log"

spawn_client() {
  UMBRIEL_LOG_KEYMAPS=1 "$CLIENT" "$1" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

active_title() {
  "$UMBRIEL" windows --json | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end'
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"

[keybinds]
"Mod+WheelDown" = "workspace-next"
"Alt" = "workspace-next"
"Super" = "workspace-next"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client "input-first"
wait_for_count 1
"$UMBRIEL" msg workspace-switch:2 > /dev/null
sleep 0.1
spawn_client "input-second"
wait_for_count 2
"$UMBRIEL" msg workspace-switch:1 > /dev/null
sleep 0.1

# Create the physical harness keyboard first, then pause while the input method creates its own current keyboard and
# grabs the physical one. This is the ordering used by input methods that inject composed text through a virtual device.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" lock num pause 1000 mod logo notch 1 mod none tap 30 > "$POINTER_LOG" 2>&1 &
POINTER_PID=$!
sleep 0.1
"$INPUT_METHOD" > "$INPUT_METHOD_LOG" 2>&1 &
INPUT_METHOD_PID=$!

for _ in $(seq 40); do
  grep -qx 'grabbed' "$INPUT_METHOD_LOG" 2>/dev/null && break
  sleep 0.05
done
if ! grep -qx 'grabbed' "$INPUT_METHOD_LOG" 2>/dev/null; then
  echo "input method did not establish its keyboard grab: $(< "$INPUT_METHOD_LOG")"
  exit 1
fi
for _ in $(seq 40); do
  [[ $(grep -c '^keymap$' "$FIRST_LOG") -gt 1 ]] && break
  sleep 0.01
done
if [[ $(grep -c '^keymap$' "$FIRST_LOG") -ne 1 ]]; then
  echo "input method's mirrored virtual keyboard changed the client keymap: $(< "$FIRST_LOG")"
  exit 1
fi

wait "$POINTER_PID" || {
  echo "pointer client failed: $(< "$POINTER_LOG")"
  exit 1
}
if [[ $(active_title) != input-second ]]; then
  echo "Mod+WheelDown ignored the physical modifier during an input-method grab: $("$UMBRIEL" windows --json)"
  exit 1
fi

for _ in $(seq 40); do
  grep -q '^key 30 1$' "$SECOND_LOG" 2>/dev/null && break
  sleep 0.05
done
if ! grep -q '^key 30 1$' "$SECOND_LOG" 2>/dev/null; then
  echo "keyboard input did not follow focus to workspace 2: first=$(< "$FIRST_LOG") second=$(< "$SECOND_LOG")"
  exit 1
fi
if grep -q '^key 30 1$' "$FIRST_LOG" 2>/dev/null; then
  echo "keyboard input remained on workspace 1 after switching: first=$(< "$FIRST_LOG") second=$(< "$SECOND_LOG")"
  exit 1
fi

# Observe the IME echo while Super is held, then verify the release action.
"$UMBRIEL" msg workspace-switch:1 > /dev/null
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" key-press 125 pause 1000 key-release 125 > "$POINTER_LOG" 2>&1 &
POINTER_PID=$!
for _ in $(seq 40); do
  grep -q '^key 125 1$' "$FIRST_LOG" 2>/dev/null && break
  sleep 0.01
done
if ! grep -q '^key 125 1$' "$FIRST_LOG" || [[ $(active_title) != input-first ]]; then
  echo "expected the IME's Super press echo on workspace 1 before releasing the modifier"
  exit 1
fi
wait "$POINTER_PID"
if [[ $(active_title) != input-second ]]; then
  echo "IME echo cancelled the modifier tap instead of switching to workspace 2 on release"
  exit 1
fi

# An unrelated virtual keyboard's key press still cancels a held modifier tap.
"$UMBRIEL" msg workspace-switch:1 > /dev/null
presses_before=$(grep -c '^key 56 1$' "$FIRST_LOG" || true)
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" key-press 56 pause 1000 key-release 56 > "$POINTER_LOG" 2>&1 &
POINTER_PID=$!
for _ in $(seq 40); do
  [[ $(grep -c '^key 56 1$' "$FIRST_LOG" || true) -gt $presses_before ]] && break
  sleep 0.01
done
if [[ $(grep -c '^key 56 1$' "$FIRST_LOG" || true) -le $presses_before ]]; then
  echo "input method did not echo the held Alt key"
  exit 1
fi
# Keep this keyboard alive past the release so its destruction cannot cancel the tap.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" tap 30 pause 1200
wait "$POINTER_PID"
if [[ $(active_title) != input-first ]]; then
  echo "an unrelated virtual keyboard's key press failed to cancel the modifier tap"
  exit 1
fi

keymaps_before_removal=$(grep -c '^keymap$' "$FIRST_LOG")
kill "$INPUT_METHOD_PID"
wait "$INPUT_METHOD_PID" || true
for _ in $(seq 40); do
  [[ $(grep -c '^keymap$' "$FIRST_LOG") -gt $keymaps_before_removal ]] && break
  sleep 0.01
done
if [[ $(grep -c '^keymap$' "$FIRST_LOG") -ne $keymaps_before_removal ]]; then
  echo "removing the input method's mirrored keyboard changed the client keymap: $(< "$FIRST_LOG")"
  exit 1
fi
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" tap 56
if [[ $(active_title) != input-second ]]; then
  echo "virtual keyboard modifier tap did not switch to workspace 2 without an input method"
  exit 1
fi
"$UMBRIEL" msg workspace-switch:1 > /dev/null

echo "input-method grabs preserve modifier wheel bindings, taps, cancellation, and keyboard focus"
