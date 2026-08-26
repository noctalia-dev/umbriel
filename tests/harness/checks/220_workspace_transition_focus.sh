#!/usr/bin/env bash
# An outgoing workspace remains visible during its slide, but it must be visual only. With focus-follows-mouse enabled,
# a tiny pointer motion over that outgoing view used to focus it and reactivate its workspace. This reproduces the
# reported Mod+Wheel workspace switching failure with the real virtual pointer and modifier paths.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"

# The wrapper survives only to pin the output dimensions every pointer call needs.
pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 &
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

[input.focus]
follows_mouse = true

[keybinds]
"Mod+WheelDown" = "workspace-next"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client "transition-first"
wait_for_count 1
"$UMBRIEL" msg workspace-switch:2 > /dev/null
sleep 0.1
spawn_client "transition-second"
wait_for_count 2
"$UMBRIEL" msg workspace-switch:1 > /dev/null
sleep 0.1

# Slow only the transition under test. The setup switches stay fast so this check does not spend twenty seconds waiting
# for animations that are unrelated to the assertion.
sed -i 's/duration_ms = 1/duration_ms = 10000/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

windows=$("$UMBRIEL" windows --json)
first_x=$(jq -r '.[] | select(.title == "transition-first") | (.x + .w / 2 | round)' <<< "$windows")
first_y=$(jq -r '.[] | select(.title == "transition-first") | (.y + .h / 2 | round)' <<< "$windows")
pointer move "$first_x" "$first_y"

# Select workspace 2 through the reported binding. Keep this assertion separate from the motion so a broken wheel bind
# cannot masquerade as the transition hit-test failure.
pointer mod logo notch 1 mod none
if [[ $(active_title) != transition-second ]]; then
  echo "Mod+WheelDown did not focus workspace 2: $("$UMBRIEL" windows --json)"
  exit 1
fi

# The old view still occupies this point near the beginning of the slide. Clicking it must not make that visual snapshot
# interactive and switch back to workspace 1.
pointer click "$BTN_LEFT"
if [[ $(active_title) != transition-second ]]; then
  echo "outgoing transition view stole focus after a click: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "outgoing workspace transition views are non-interactive"
