#!/usr/bin/env bash
# A dialog opens centered on what its parent shows: the whole output for a fullscreen parent and the usable area for
# one maximized to edges, not the tile underneath either.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly BASE_CONFIG="$UMBRIEL_RUNTIME_DIR/transient-fullscreen-base.toml"
readonly PANEL_LOG="$UMBRIEL_RUNTIME_DIR/transient-fullscreen-panel.log"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/transient-fullscreen.log"

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

# $1 is the rule that opens the parent in the state under test. A left strut keeps the tiles away from the output's
# corner, so a dialog centered on the parent's tile lands elsewhere than one centered on the output or the usable area.
write_config() {
  cat "$BASE_CONFIG" > "$UMBRIEL_CONFIG"
  cat >> "$UMBRIEL_CONFIG" <<EOF2

[animation]
enabled = false

[layout]
mode = "dwindle"

[layout.struts]
left = 100

[[window_rule]]
match.title = "^transient-parent$"
$1 = true
EOF2
  "$UMBRIEL" msg config-reload > /dev/null
}

cp "$UMBRIEL_CONFIG" "$BASE_CONFIG"

# A 40 pixel panel takes the top of the usable area.
"$LAYER_CLIENT" HEADLESS-1 40 > "$PANEL_LOG" 2>&1 &
for _ in $(seq 80); do
  if grep -q '^ready$' "$PANEL_LOG"; then
    break
  fi
  sleep 0.05
done
if ! grep -q '^ready$' "$PANEL_LOG"; then
  echo "exclusive-zone panel did not map: $(< "$PANEL_LOG")"
  exit 1
fi

# Fullscreen covers the whole 1280x720 output, so the 400x300 dialog opens at 440,210.
write_config default_fullscreen
TRANSIENT_SUITE=1 "$CLIENT" transient-child 400 300 > "$CLIENT_LOG" 2>&1 &
client_pid=$!
wait_for_window_count 3
wait_for_position transient-child 440 210

kill "$client_pid"
wait "$client_pid" 2> /dev/null || true
wait_for_window_count 0

# Maximized to edges fills the usable area below the panel, 1280x680 at 0,40, so the dialog opens at 440,230.
write_config default_maximize_to_edges
TRANSIENT_SUITE=1 "$CLIENT" transient-child 400 300 > "$CLIENT_LOG" 2>&1 &
wait_for_window_count 3
wait_for_position transient-child 440 230

echo "a dialog opens centered on what a fullscreen or edge-maximized parent shows"
