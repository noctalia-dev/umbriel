#!/usr/bin/env bash
# When a new window changes focus and the scene under a stationary pointer, the next motion gets one hover refresh.
# Both coordinates are already inside the newly exposed window, so geometric enter detection alone cannot see it.
# A window matched by `keep_focus_on_hover`, or its attached modal dialog, skips that refresh and real enters until a
# click moves focus.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

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

wait_for_active() {
  local want=$1
  for _ in $(seq 40); do
    [[ $(active_title) == "$want" ]] && return 0
    sleep 0.05
  done
  echo "expected '$want' to be active, got: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "scrolling"

[layout.scrolling]
default_extent_fraction = 0.5

[animation]
duration_ms = 1
curve = "linear"

[input.focus]
follows_mouse = true

[[window_rule]]
match.title = "^spawn-hover-held$"
default_floating = true
default_position = { x = 440, y = 240, anchor = "top_left" }
keep_focus_on_hover = true

# The rule names the parent; its attached modal dialog takes the keyboard.
[[window_rule]]
match.title = "^transient-parent$"
default_floating = true
default_position = { x = 440, y = 240, anchor = "top_left" }
keep_focus_on_hover = true

[[window_rule]]
match.title = "^transient-unrelated$"
default_floating = true
default_position = { x = 900, y = 500, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

# This point is inside the first half-width tile once the windows map. Keep the pointer there while the second window
# maps and takes keyboard focus on the right.
pointer move 64 64
spawn_client "spawn-hover-left"
wait_for_count 1
spawn_client "spawn-hover-right"
wait_for_count 2
wait_for_active "spawn-hover-right"
sleep 0.1

# A one-pixel move stays inside the left tile. The map-time focus change must let it refresh hover focus once.
pointer move 65 64
wait_for_active "spawn-hover-left"

echo "motion focuses a window that appeared under the stationary pointer"

# The held window opens away from the pointer. Neither the map-time refresh nor entering the right tile takes focus.
spawn_client "spawn-hover-held"
wait_for_count 3
wait_for_active "spawn-hover-held"
pointer move 66 64
"$UMBRIEL" settle > /dev/null
wait_for_active "spawn-hover-held"
pointer move 1000 64
"$UMBRIEL" settle > /dev/null
wait_for_active "spawn-hover-held"

# A click still moves focus, and hover focus resumes once the held window has lost it.
pointer click 272
wait_for_active "spawn-hover-right"
pointer move 64 64
wait_for_active "spawn-hover-left"

echo "keep_focus_on_hover holds focus against hover until a click"

TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 TRANSIENT_PARENT_SIZE=400x200 "$CLIENT" modal-dialog 200 100 > /dev/null 2>&1 &
wait_for_count 6
wait_for_active "modal-dialog"
pointer move 65 64
"$UMBRIEL" settle > /dev/null
wait_for_active "modal-dialog"
pointer move 1000 64
"$UMBRIEL" settle > /dev/null
wait_for_active "modal-dialog"

echo "a held window's attached modal dialog keeps focus against hover"
