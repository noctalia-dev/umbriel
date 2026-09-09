#!/usr/bin/env bash
# A keybind consumes both halves of its key event, so the surface it focuses
# must not be handed that key as held: nothing would ever release it, and
# XWayland turns a held key into an endless repeat.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly KEY_L=38
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly LEFT_LOG="$UMBRIEL_RUNTIME_DIR/bind-key-left.log"
readonly RIGHT_LOG="$UMBRIEL_RUNTIME_DIR/bind-key-right.log"

if [[ ! -x $POINTER || ! -x $OBSERVER ]]; then
  echo "input helpers are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[keybinds]
"Mod+L" = "window-focus-right"
EOF
"$UMBRIEL" msg config-reload > /dev/null

await_windows() {
  local expected=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected windows, got $("$UMBRIEL" windows --json | jq -c 'map(.title)')"
  return 1
}

focused_title() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.focused) | .title'
}

"$OBSERVER" bind-key-left > "$LEFT_LOG" 2>&1 &
await_windows 1
"$OBSERVER" bind-key-right > "$RIGHT_LOG" 2>&1 &
await_windows 2

# Start from the left window without a key having been involved. The headless
# harness has no keyboard until a virtual one appears, so focus is read from the
# compositor rather than from a keyboard enter.
"$UMBRIEL" msg window-focus-left > /dev/null
sleep 0.3
if [[ $(focused_title) != 'bind-key-left' ]]; then
  echo "expected the left window focused, got '$(focused_title)'"
  exit 1
fi

# Mod+L runs window-focus-right, so the right window takes keyboard focus while
# L is still physically down. The plain L that follows is the positive control,
# and it runs on the same connection because the seat only has a keyboard while
# this client keeps its virtual one alive.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  mod logo pause 300 tap "$KEY_L" mod none pause 300 tap "$KEY_L"
sleep 0.3
if [[ $(focused_title) != 'bind-key-right' ]]; then
  echo "the bind did not move focus to the right window, got '$(focused_title)'"
  exit 1
fi

if ! grep -q 'keyboard-enter' "$RIGHT_LOG"; then
  echo "the right window never received a keyboard enter"
  exit 1
fi
if grep 'keyboard-enter' "$RIGHT_LOG" | grep -qv 'keys=0'; then
  echo "focus enter handed the bind key over as held: $(grep 'keyboard-enter' "$RIGHT_LOG")"
  exit 1
fi
if grep -q "keyboard-key code=$KEY_L" "$LEFT_LOG"; then
  echo "the consumed bind key reached the outgoing window: $(grep "keyboard-key code=$KEY_L" "$LEFT_LOG")"
  exit 1
fi

# The bind consumed one press and one release, so only the control's pair is
# left. Counting is what separates a working filter from one that also eats
# ordinary typing.
presses=$(grep -c "keyboard-key code=$KEY_L state=pressed" "$RIGHT_LOG" || true)
releases=$(grep -c "keyboard-key code=$KEY_L state=released" "$RIGHT_LOG" || true)
if ((presses != 1 || releases != 1)); then
  echo "expected one press and one release from the unbound key, got $presses and $releases"
  echo "log: $(tr '\n' '|' < "$RIGHT_LOG")"
  exit 1
fi

echo "bind-driven focus hands over no held key and plain typing still arrives"
