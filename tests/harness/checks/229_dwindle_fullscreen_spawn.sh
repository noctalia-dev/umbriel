#!/usr/bin/env bash
# In dwindle, a new tiled window maps below an existing fullscreen window. The
# fullscreen action must leave the visible fullscreen state instead of granting
# fullscreen to the obscured new window and stacking two fullscreen surfaces.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly FIRST_LOG="$UMBRIEL_RUNTIME_DIR/dwindle-fullscreen-first.log"
readonly SECOND_LOG="$UMBRIEL_RUNTIME_DIR/dwindle-fullscreen-second.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "dwindle"

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_query() {
  local query=$1 message=$2 windows=
  for _ in $(seq 60); do
    windows=$("$UMBRIEL" windows --json)
    jq -e "$query" <<< "$windows" > /dev/null && return 0
    sleep 0.1
  done
  echo "$message: $windows"
  return 1
}

wait_for_fullscreen_query() {
  local query=$1 message=$2 state=
  for _ in $(seq 60); do
    state=$("$UMBRIEL" tearing --json)
    jq -e "$query" <<< "$state" > /dev/null && return 0
    sleep 0.1
  done
  echo "$message: $state"
  return 1
}

"$CLIENT" dwindle-fullscreen-first > "$FIRST_LOG" 2>&1 &
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-first")] | length == 1' \
  "first window did not map"

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen_query \
  '[.surfaces[] | select(.title == "dwindle-fullscreen-first" and .fullscreen)] | length == 1' \
  "first window did not enter fullscreen"

"$CLIENT" dwindle-fullscreen-second > "$SECOND_LOG" 2>&1 &
wait_for_query \
  'length == 2 and ([.[] | select(.title == "dwindle-fullscreen-second")] | length == 1)' \
  "second window did not map"
wait_for_query \
  '([.[] | select(.title == "dwindle-fullscreen-first") | .focused] == [false])
    and ([.[] | select(.title == "dwindle-fullscreen-second") | .focused] == [true])' \
  "second window did not take focus beneath the fullscreen window"
wait_for_fullscreen_query \
  '([.surfaces[] | select(.title == "dwindle-fullscreen-first") | .fullscreen] == [true])
    and ([.surfaces[] | select(.title == "dwindle-fullscreen-second") | .fullscreen] == [false])' \
  "mapping the second window changed the fullscreen state"

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen_query \
  '(.surfaces | length) == 2 and all(.surfaces[]; .fullscreen == false)' \
  "fullscreen toggle did not leave the visible fullscreen state"
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-second" and .focused)] | length == 1' \
  "leaving the obscuring fullscreen changed focus"

echo "a new dwindle window cannot redirect the fullscreen exit toggle"
