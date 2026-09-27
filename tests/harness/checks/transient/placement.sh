#!/usr/bin/env bash
# A parented XDG toplevel without a position rule opens centered over its parent, not over the output.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/transient-placement.log"

windows() {
  "$UMBRIEL" windows --json
}

wait_for_window_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $(windows)"
  return 1
}

wait_for_position() {
  local title=$1 x=$2 y=$3
  for _ in $(seq 60); do
    if [[ $(windows | jq -r --arg title "$title" '.[] | select(.title == $title) | "\(.x) \(.y)"') == "$x $y" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $title at $x,$y, got: $(windows)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[window_rule]]
match.title = "^transient-parent$"
default_floating = true
default_position = { x = 100, y = 110, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Over the 900x600 parent the 400x300 child lands at 350,260. Centered on the 1280x720 output it would be 440,210.
TRANSIENT_SUITE=1 TRANSIENT_PARENT_SIZE=900x600 "$CLIENT" transient-child 400 300 > "$CLIENT_LOG" 2>&1 &
wait_for_window_count 3
wait_for_position transient-parent 100 110
wait_for_position transient-child 350 260

echo "transient dialogs open centered over their parent"
