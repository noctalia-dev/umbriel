#!/usr/bin/env bash
# harness: outputs=2
# A named scrolling-column creator keeps width ownership after its output disappears and returns.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly OWNER_FIFO="$UMBRIEL_RUNTIME_DIR/named-output-owner-control"

spawn_client() {
  "$CLIENT" "$1" 800 600 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
}

wait_for_windows() {
  local expected=$1
  for _ in $(seq 80); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $expected ]] && return 0
    sleep 0.1
  done
  echo "timed out waiting for $expected clients: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_output() {
  local title=$1 expected=$2 output=
  for _ in $(seq 80); do
    output=$("$UMBRIEL" windows --json | jq -r --arg title "$title" \
      '.[] | select(.title == $title) | .workspace | split(":")[0]')
    [[ $output == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on '$expected', got '$output'"
  return 1
}

wait_for_delta() {
  local first=$1 second=$2 expected=$3 windows first_x second_x
  for _ in $(seq 80); do
    windows=$("$UMBRIEL" windows --json)
    first_x=$(jq -r --arg title "$first" '.[] | select(.title == $title) | .x' <<< "$windows")
    second_x=$(jq -r --arg title "$second" '.[] | select(.title == $title) | .x' <<< "$windows")
    [[ -n $first_x && -n $second_x ]] && ((second_x - first_x == expected)) && return 0
    sleep 0.1
  done
  echo "restored column owner did not apply its late width: $windows"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"

[output.HEADLESS-1]
position = [0, 0]

[output.HEADLESS-2]
position = [1280, 0]

[[window_rule]]
match.app_id = "^named-output-owner$"
default_scrolling_column = "shared-refuge"
default_output = "HEADLESS-1"

[[window_rule]]
match.title = "^named-output-owner-late$"
default_scrolling_extent = 0.7

[[window_rule]]
match.title = "^named-output-peer$"
default_scrolling_column = "shared-refuge"
default_output = "HEADLESS-1"

[[window_rule]]
match.title = "^named-output-refuge$"
default_scrolling_column = "shared-refuge"
default_output = "HEADLESS-2"

[[window_rule]]
match.title = "^named-output-after$"
default_output = "HEADLESS-1"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client named-output-refuge
wait_for_windows 1
mkfifo "$OWNER_FIFO"
exec {owner_fd}<>"$OWNER_FIFO"
APP_ID=named-output-owner TITLE_AFTER_MAP=named-output-owner-late \
  "$CLIENT" named-output-owner-placeholder 800 600 <&"$owner_fd" \
  > "$UMBRIEL_RUNTIME_DIR/named-output-owner.log" 2>&1 &
wait_for_windows 2
spawn_client named-output-peer
wait_for_windows 3

# The home column temporarily joins a populated, identically named refuge.
# Settle its late width there, then verify exact restoration retains both the
# creator's ownership and the newly resolved width.
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
wait_for_output named-output-owner-placeholder HEADLESS-2
printf 'u' >&"$owner_fd"
wait_for_output named-output-owner-late HEADLESS-2
created=$("$UMBRIEL" output-create HEADLESS-1)
[[ $created == HEADLESS-1 ]] || { echo "expected HEADLESS-1, got '$created'"; exit 1; }
wait_for_output named-output-owner-late HEADLESS-1

spawn_client named-output-after
wait_for_windows 4
wait_for_delta named-output-owner-late named-output-after 890

echo "named-column width ownership survived output loss and exact layout restoration"
