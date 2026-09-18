#!/usr/bin/env bash
# A focused client may inhibit ordinary compositor keyboard shortcuts while an
# explicitly exempt escape binding remains available to the user.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly KEY_1=2
readonly KEY_2=3
readonly KEY_ESC=1
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/shortcuts-inhibit.log"
readonly OTHER_LOG="$UMBRIEL_RUNTIME_DIR/shortcuts-other.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[keybinds]
"Mod+1" = "workspace-set-layout:master"
"Mod+2" = { action = "workspace-set-layout:dwindle", allow_when_inhibited = true }
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

INHIBIT_SHORTCUTS=1 "$OBSERVER" shortcuts-inhibit > "$CLIENT_LOG" 2>&1 &
inhibitor_pid=$!
wait_for "the test window" '[[ $("$UMBRIEL" windows --json | jq length) == 1 ]]'
wait_for "the inhibitor to activate" 'grep -q "shortcuts-inhibitor active" "$CLIENT_LOG"'

# The ordinary bind is suppressed and both key event halves reach the client.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" mod logo tap "$KEY_1" mod none
sleep 0.2
if [[ $(focused_layout) != scrolling ]]; then
  echo "an inhibited ordinary binding changed the layout to $(focused_layout)"
  exit 1
fi
presses=$(grep -c "keyboard-key code=$KEY_1 state=pressed" "$CLIENT_LOG" || true)
releases=$(grep -c "keyboard-key code=$KEY_1 state=released" "$CLIENT_LOG" || true)
if ((presses != 1 || releases != 1)); then
  echo "expected the inhibited key press and release at the client, got $presses and $releases"
  exit 1
fi

# An exempt bind still belongs to the compositor and neither half leaks.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" mod logo tap "$KEY_2" mod none
wait_for "the exempt binding" '[[ $(focused_layout) == dwindle ]]'
if grep -q "keyboard-key code=$KEY_2" "$CLIENT_LOG"; then
  echo "the exempt binding leaked a key event to the client"
  exit 1
fi

# The escape binding deactivates the protocol object, restoring normal binds.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" mod logo tap "$KEY_ESC" mod none
wait_for "the inhibitor to deactivate" 'grep -q "shortcuts-inhibitor inactive" "$CLIENT_LOG"'
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" mod logo tap "$KEY_1" mod none
wait_for "ordinary bindings to resume" '[[ $(focused_layout) == master ]]'

# The same binding reactivates the focused inhibitor without leaking Escape.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" mod logo tap "$KEY_ESC" mod none
wait_for "the inhibitor to reactivate" '[[ $(grep -c "shortcuts-inhibitor active" "$CLIENT_LOG") == 2 ]]'
if grep -q "keyboard-key code=$KEY_ESC" "$CLIENT_LOG"; then
  echo "the inhibitor toggle leaked Escape to the client"
  exit 1
fi

# Event ownership is decided by the press. Turning inhibition off while an
# application-owned key is held still forwards its release. Turning inhibition
# on after a compositor-owned press still suppresses that release.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  mod logo key-press "$KEY_1" tap "$KEY_ESC" key-release "$KEY_1" mod none
wait_for "deactivation during a held application key" '[[ $(grep -c "shortcuts-inhibitor inactive" "$CLIENT_LOG") == 2 ]]'
presses=$(grep -c "keyboard-key code=$KEY_1 state=pressed" "$CLIENT_LOG" || true)
releases=$(grep -c "keyboard-key code=$KEY_1 state=released" "$CLIENT_LOG" || true)
if ((presses != 2 || releases != 2)); then
  echo "changing inhibition stranded an application key: got $presses presses and $releases releases"
  exit 1
fi

"$UMBRIEL" msg workspace-set-layout:scrolling > /dev/null
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  mod logo key-press "$KEY_1" tap "$KEY_ESC" key-release "$KEY_1" mod none
wait_for "reactivation during a held compositor key" '[[ $(grep -c "shortcuts-inhibitor active" "$CLIENT_LOG") == 3 ]]'
if [[ $(focused_layout) != master ]]; then
  echo "the compositor-owned press did not run before inhibition resumed"
  exit 1
fi
presses=$(grep -c "keyboard-key code=$KEY_1 state=pressed" "$CLIENT_LOG" || true)
releases=$(grep -c "keyboard-key code=$KEY_1 state=released" "$CLIENT_LOG" || true)
if ((presses != 2 || releases != 2)); then
  echo "a compositor-owned key leaked after inhibition resumed"
  exit 1
fi

# An inhibitor is relevant only while its exact surface holds keyboard focus.
"$OBSERVER" shortcuts-other > "$OTHER_LOG" 2>&1 &
wait_for "the uninhibited test window" '[[ $("$UMBRIEL" windows --json | jq length) == 2 ]]'
wait_for "the uninhibited surface to gain focus" \
  '[[ $("$UMBRIEL" windows --json | jq -r '\''.[] | select(.focused) | .title'\'') == shortcuts-other ]]'
"$UMBRIEL" msg workspace-set-layout:scrolling > /dev/null
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" mod logo tap "$KEY_1" mod none
wait_for "the inhibitor to become irrelevant off focus" '[[ $(focused_layout) == master ]]'
if grep -q "keyboard-key code=$KEY_1" "$OTHER_LOG"; then
  echo "an ordinary bind was forwarded while a different surface held focus"
  exit 1
fi

# Destroying the inhibitor client leaves ordinary shortcut handling intact.
kill "$inhibitor_pid"
wait_for "the inhibitor window to close" '[[ $("$UMBRIEL" windows --json | jq length) == 1 ]]'
"$UMBRIEL" msg workspace-set-layout:scrolling > /dev/null
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" mod logo tap "$KEY_1" mod none
wait_for "shortcuts after inhibitor destruction" '[[ $(focused_layout) == master ]]'

echo "focused shortcut inhibition forwards ordinary keys and preserves explicit escape bindings"
