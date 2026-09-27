#!/usr/bin/env bash
# In dwindle, a new tiled window normally maps below an existing fullscreen window. The fullscreen action must still
# leave the visible fullscreen state. With new_exits_fullscreen enabled, only a window arriving on the workspace exits
# fullscreen: opening it there, a late title rule completing its initial placement, or moving it in from another
# workspace. Attaching an existing floating window to the layout, or moving a window to the workspace it is already
# on, leaves fullscreen unchanged. The scope selects which kinds of arriving window count.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly FIRST_LOG="$UMBRIEL_RUNTIME_DIR/dwindle-fullscreen-first.log"
readonly SECOND_LOG="$UMBRIEL_RUNTIME_DIR/dwindle-fullscreen-second.log"
readonly THIRD_LOG="$UMBRIEL_RUNTIME_DIR/dwindle-fullscreen-third.log"
readonly TARGET_LOG="$UMBRIEL_RUNTIME_DIR/dwindle-fullscreen-target.log"
readonly LATE_LOG="$UMBRIEL_RUNTIME_DIR/dwindle-fullscreen-late.log"
readonly TITLE_FIFO="$UMBRIEL_RUNTIME_DIR/dwindle-fullscreen-title.fifo"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "dwindle"
new_exits_fullscreen = []

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

sed -i 's/^new_exits_fullscreen = \[\]$/new_exits_fullscreen = "tiled"/' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output.HEADLESS-1]
workspaces = 2

[[window_rule]]
match.title = "^dwindle-fullscreen-late$"
default_workspace = 2
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Attaching an existing view to the layout is not a new window opening.
"$UMBRIEL" msg window-toggle-floating > /dev/null
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-second" and .floating)] | length == 1' \
  "second window did not become floating"
first_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "dwindle-fullscreen-first") | .id')
second_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "dwindle-fullscreen-second") | .id')
"$UMBRIEL" msg "window-focus:$first_id" > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen_query \
  '[.surfaces[] | select(.title == "dwindle-fullscreen-first" and .fullscreen)] | length == 1' \
  "first window did not re-enter fullscreen"
"$UMBRIEL" msg "window-focus:$second_id" > /dev/null
"$UMBRIEL" msg window-toggle-floating > /dev/null
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-second" and (.floating | not))] | length == 1' \
  "second window did not reattach to the layout"
wait_for_fullscreen_query \
  '[.surfaces[] | select(.title == "dwindle-fullscreen-first" and .fullscreen)] | length == 1' \
  "retiling an existing window changed the fullscreen state"

"$CLIENT" dwindle-fullscreen-third > "$THIRD_LOG" 2>&1 &
wait_for_query \
  'length == 3 and ([.[] | select(.title == "dwindle-fullscreen-third")] | length == 1)' \
  "third window did not map"
wait_for_fullscreen_query \
  '(.surfaces | length) == 3 and all(.surfaces[]; .fullscreen == false)' \
  "mapping a new window did not exit fullscreen"

"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$CLIENT" dwindle-fullscreen-target > "$TARGET_LOG" 2>&1 &
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-target" and .active and .focused)] | length == 1' \
  "target workspace window did not map"
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen_query \
  '[.surfaces[] | select(.title == "dwindle-fullscreen-target" and .fullscreen)] | length == 1' \
  "target workspace window did not enter fullscreen"
"$UMBRIEL" msg workspace-switch:1 > /dev/null

mkfifo "$TITLE_FIFO"
exec {title_fd}<>"$TITLE_FIFO"
TITLE_AFTER_MAP=dwindle-fullscreen-late \
  "$CLIENT" dwindle-fullscreen-placeholder 800 600 <&"$title_fd" > "$LATE_LOG" 2>&1 &
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-placeholder" and (.workspace | endswith(":1")))] | length == 1' \
  "late-title window did not initially map on workspace 1"
printf 'u' >&"$title_fd"
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-late" and (.workspace | endswith(":2")))] | length == 1' \
  "late title rule did not move the new window to workspace 2"
wait_for_fullscreen_query \
  '(.surfaces | length) == 5 and all(.surfaces[]; .fullscreen == false)' \
  "late initial workspace placement did not exit fullscreen"

target_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "dwindle-fullscreen-target") | .id')
third_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "dwindle-fullscreen-third") | .id')
late_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "dwindle-fullscreen-late") | .id')

"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" msg "window-focus:$target_id" > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen_query \
  '[.surfaces[] | select(.title == "dwindle-fullscreen-target" and .fullscreen)] | length == 1' \
  "target window did not re-enter fullscreen"
"$UMBRIEL" msg workspace-switch:1 > /dev/null
"$UMBRIEL" msg "window-focus:$third_id" > /dev/null
"$UMBRIEL" msg window-move-to-workspace:2 > /dev/null
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-third" and (.workspace | endswith(":2")))] | length == 1' \
  "third window did not move to workspace 2"
wait_for_fullscreen_query \
  '[.surfaces[] | select(.title == "dwindle-fullscreen-target") | .fullscreen] == [false]' \
  "moving a tiled window in did not exit fullscreen"

sed -i 's/^new_exits_fullscreen = "tiled"$/new_exits_fullscreen = "floating"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

"$UMBRIEL" msg "window-focus:$late_id" > /dev/null
"$UMBRIEL" msg window-toggle-floating > /dev/null
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-late" and .floating)] | length == 1' \
  "late window did not become floating"
"$UMBRIEL" msg "window-focus:$target_id" > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen_query \
  '[.surfaces[] | select(.title == "dwindle-fullscreen-target" and .fullscreen)] | length == 1' \
  "target window did not enter fullscreen before the floating moves"

# A move onto the workspace the window already occupies is not an arrival. settle waits for every configure an
# erroneous exit would send to be acknowledged, so the fullscreen state read after it is final.
"$UMBRIEL" msg "window-focus:$late_id" > /dev/null
"$UMBRIEL" msg window-move-to-workspace:2 > /dev/null
"$UMBRIEL" settle
state=$("$UMBRIEL" tearing --json)
if ! jq -e '[.surfaces[] | select(.title == "dwindle-fullscreen-target") | .fullscreen] == [true]' <<< "$state" \
  > /dev/null; then
  echo "moving a floating window to its own workspace exited fullscreen: $state"
  exit 1
fi

"$UMBRIEL" msg window-move-to-workspace:1 > /dev/null
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-late" and (.workspace | endswith(":1")))] | length == 1' \
  "late window did not move to workspace 1"
"$UMBRIEL" msg window-move-to-workspace:2 > /dev/null
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-late" and (.workspace | endswith(":2")))] | length == 1' \
  "late window did not move back to workspace 2"
wait_for_fullscreen_query \
  '[.surfaces[] | select(.title == "dwindle-fullscreen-target") | .fullscreen] == [false]' \
  "moving a floating window in did not exit fullscreen"

# On a scrolling workspace a tiled arrival opens beside the fullscreen column, so even "all" keeps fullscreen.
sed -i 's/^new_exits_fullscreen = "floating"$/new_exits_fullscreen = "all"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg workspace-set-layout:scrolling > /dev/null
"$UMBRIEL" msg "window-focus:$target_id" > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen_query \
  '[.surfaces[] | select(.title == "dwindle-fullscreen-target" and .fullscreen)] | length == 1' \
  "target window did not enter fullscreen on the scrolling workspace"
"$UMBRIEL" msg "window-focus:$third_id" > /dev/null
"$UMBRIEL" msg window-move-to-workspace:1 > /dev/null
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-third" and (.workspace | endswith(":1")))] | length == 1' \
  "third window did not move to workspace 1"
"$UMBRIEL" msg window-move-to-workspace:2 > /dev/null
wait_for_query \
  '[.[] | select(.title == "dwindle-fullscreen-third" and (.workspace | endswith(":2")))] | length == 1' \
  "third window did not move back to the scrolling workspace"
"$UMBRIEL" settle
state=$("$UMBRIEL" tearing --json)
if ! jq -e '[.surfaces[] | select(.title == "dwindle-fullscreen-target") | .fullscreen] == [true]' <<< "$state" \
  > /dev/null; then
  echo "a tiled arrival on a scrolling workspace exited fullscreen: $state"
  exit 1
fi

echo "only windows arriving on a workspace exit fullscreen when configured, by scope; scrolling keeps it for tiles"
