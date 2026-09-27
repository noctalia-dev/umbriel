#!/usr/bin/env bash
# harness: outputs=1
# A same-workspace floating drag must commit its final scene position as the
# layout target so maximize restores the exact dropped box.
set -euo pipefail

readonly BTN_LEFT=272
readonly TITLE=floating-drag-maximize-restore
readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/floating-drag-maximize-restore.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[[window_rule]]
match.title = "^floating-drag-maximize-restore$"
default_floating = true
default_floating_size_px = { width = 480, height = 300 }
default_position = { x = 173, y = 109, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

ipc_box() {
  "$UMBRIEL" windows --json \
    | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | "\(.w)x\(.h)+\(.x)+\(.y)"'
}

wait_for_box() {
  local expected=$1 actual=
  for _ in $(seq 100); do
    actual=$(ipc_box)
    [[ $actual == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected floating box $expected, got $actual"
  return 1
}

"$CLIENT" "$TITLE" 480 300 > "$CLIENT_LOG" 2>&1 &
wait_for_box 480x300+173+109

window_id=$("$UMBRIEL" windows --json | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | .id')
"$UMBRIEL" msg "window-focus-warp:$window_id" > /dev/null

# Grab the center and move by exactly +320,+180 without changing workspace.
"$POINTER" 1280 720 move 413 259 mod logo press "$BTN_LEFT" move 733 439 release "$BTN_LEFT" mod none
wait_for_box 480x300+493+289

"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_box 1280x720+0+0

"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_box 480x300+493+289

echo "floating maximize restored the exact same-workspace drag position"
