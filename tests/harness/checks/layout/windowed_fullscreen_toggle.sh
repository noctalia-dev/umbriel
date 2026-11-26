#!/usr/bin/env bash
# harness: outputs=1
# Windowed fullscreen tells the client it is fullscreen while the window keeps its tile, so a video player can fill a
# column. The client's own fullscreen requests stay in the tile, its unfullscreen request leaves the state, and real
# fullscreen replaces it.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/windowed-fullscreen-control"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/windowed-fullscreen.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

field() {
  "$UMBRIEL" windows --json | jq -r --arg field "$1" '.[] | select(.title == "windowed-video") | .[$field]'
}

expect_field() {
  local name=$1 want=$2 got=
  got=$(field "$name")
  if [[ $got != "$want" ]]; then
    echo "expected $name=$want, got $got: $("$UMBRIEL" windows --json)"
    exit 1
  fi
}

wait_for_configured_state() {
  local want=$1 configured=
  for _ in $(seq 60); do
    configured=$(grep '^configured-state=' "$CLIENT_LOG" | tail -1 || true)
    [[ $configured == "configured-state=$want" ]] && return 0
    sleep 0.05
  done
  echo "expected latest client configure to be '$want', got '${configured:-none}': $(cat "$CLIENT_LOG")"
  exit 1
}

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
FULLSCREEN_ON_STDIN=1 LOG_CONFIGURES=1 FOLLOW_CONFIGURES=1 "$CLIENT" windowed-video \
  <&"$control_fd" > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 60); do
  [[ -n $(field id) ]] && break
  sleep 0.05
done
"$UMBRIEL" settle
tile="$(field w)x$(field h)"
if [[ $tile == "1280x720" ]]; then
  echo "the tile must be smaller than the output for this check to mean anything: $tile"
  exit 1
fi
wait_for_configured_state "$tile windowed"

"$UMBRIEL" msg window-toggle-windowed-fullscreen > /dev/null
wait_for_configured_state "$tile fullscreen"
"$UMBRIEL" settle
expect_field windowed_fullscreen true
expect_field fullscreen false
expect_field w "${tile%x*}"

# The client leaving fullscreen leaves windowed fullscreen.
printf u >&"$control_fd"
wait_for_configured_state "$tile windowed"
"$UMBRIEL" settle
expect_field windowed_fullscreen false
expect_field fullscreen false

# The client asking for fullscreen while windowed fullscreen stays in its tile.
"$UMBRIEL" msg window-toggle-windowed-fullscreen > /dev/null
wait_for_configured_state "$tile fullscreen"
printf f >&"$control_fd"
"$UMBRIEL" settle
wait_for_configured_state "$tile fullscreen"
expect_field windowed_fullscreen true
expect_field fullscreen false

# Real fullscreen replaces windowed fullscreen, and leaving it returns to the tile.
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_configured_state "1280x720 fullscreen"
"$UMBRIEL" settle
expect_field windowed_fullscreen false
expect_field fullscreen true

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_configured_state "$tile windowed"
"$UMBRIEL" settle
expect_field windowed_fullscreen false
expect_field fullscreen false

echo "windowed fullscreen keeps the tile while the client sees fullscreen"
