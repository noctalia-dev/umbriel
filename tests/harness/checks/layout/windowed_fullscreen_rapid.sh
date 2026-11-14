#!/usr/bin/env bash
# harness: outputs=1
# Windowed fullscreen settles on the latest state when transitions arrive while the client still owes acks for
# earlier configures, including real fullscreen back to windowed fullscreen before the real fullscreen size is acked.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/windowed-fullscreen-rapid-control"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/windowed-fullscreen-rapid.log"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

field() {
  "$UMBRIEL" windows --json | jq -r --arg field "$1" '.[] | select(.title == "rapid-video") | .[$field]'
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

await_line() {
  local pattern=$1
  for _ in $(seq 60); do
    grep -q "$pattern" "$CLIENT_LOG" && return 0
    sleep 0.05
  done
  echo "client never printed '$pattern': $(cat "$CLIENT_LOG")"
  exit 1
}

# The client acks the latest configure it is holding and redraws at that size. Settling before this would wait on
# the client forever.
release() {
  local size=$1 before
  before=$(grep -c "^resized $size\$" "$CLIENT_LOG" || true)
  printf r >&"$control_fd"
  for _ in $(seq 60); do
    (($(grep -c "^resized $size\$" "$CLIENT_LOG" || true) > before)) && break
    sleep 0.05
  done
  if (($(grep -c "^resized $size\$" "$CLIENT_LOG" || true) <= before)); then
    echo "client did not redraw at $size after its ack: $(cat "$CLIENT_LOG")"
    exit 1
  fi
  "$UMBRIEL" settle
}

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
FULLSCREEN_ON_STDIN=1 RESIZE_ON_STDIN=1 LOG_CONFIGURES=1 FOLLOW_CONFIGURES=1 "$CLIENT" rapid-video \
  <&"$control_fd" > "$CLIENT_LOG" 2>&1 &

# The client holds its first tile configure too; release it before reading the tile.
await_line '^resize-pending '
release "$(grep '^resize-pending ' "$CLIENT_LOG" | tail -1 | cut -d' ' -f2)"
tile="$(field w)x$(field h)"
if [[ $tile == "1280x720" ]]; then
  echo "the tile must be smaller than the output for this check to mean anything: $tile"
  exit 1
fi
wait_for_configured_state "$tile windowed"

# Real fullscreen, then windowed fullscreen before the client has acked the full-output size.
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_configured_state "1280x720 fullscreen"
# The client must still owe the real fullscreen ack for this check to mean anything.
await_line '^resize-pending 1280x720$'
"$UMBRIEL" msg window-toggle-windowed-fullscreen > /dev/null
wait_for_configured_state "$tile fullscreen"
release "$tile"
expect_field windowed_fullscreen true
expect_field fullscreen false
expect_field w "${tile%x*}"
expect_field h "${tile#*x}"

# A burst of toggles with no acks in between ends on the last one: windowed fullscreen off, real fullscreen, real off
# (back to the tile), windowed fullscreen on, then the client's own unfullscreen.
"$UMBRIEL" msg window-toggle-windowed-fullscreen > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
"$UMBRIEL" msg window-toggle-windowed-fullscreen > /dev/null
wait_for_configured_state "$tile fullscreen"
release "$tile"
expect_field windowed_fullscreen true
expect_field fullscreen false
expect_field w "${tile%x*}"

printf u >&"$control_fd"
wait_for_configured_state "$tile windowed"
"$UMBRIEL" settle
expect_field windowed_fullscreen false
expect_field fullscreen false
expect_field w "${tile%x*}"

echo "windowed fullscreen settles on the latest transition with acks outstanding"
