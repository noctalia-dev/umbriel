#!/usr/bin/env bash
# Late named scrolling-column rules preserve ownership, focus, width, and maximize state across relocation.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly MOVE_FIFO="$UMBRIEL_RUNTIME_DIR/named-move-control"
readonly MAX_MOVE_FIFO="$UMBRIEL_RUNTIME_DIR/named-max-move-control"
readonly MAX_RECLASSIFY_FIFO="$UMBRIEL_RUNTIME_DIR/named-max-reclassify-control"
readonly OWNER_WIDTH_FIFO="$UMBRIEL_RUNTIME_DIR/named-owner-width-control"

spawn_client() {
  "$CLIENT" "$1" 800 600 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
}

wait_for_windows() {
  local expected=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $expected ]] && return 0
    sleep 0.1
  done
  echo "timed out waiting for $expected clients: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_query() {
  local query=$1 message=$2
  for _ in $(seq 60); do
    if "$UMBRIEL" windows --json | jq -e "$query" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "$message: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_delta() {
  local first=$1 second=$2 expected=$3 message=$4 windows first_x second_x
  for _ in $(seq 60); do
    windows=$("$UMBRIEL" windows --json)
    first_x=$(jq -r --arg title "$first" '.[] | select(.title == $title) | .x' <<< "$windows")
    second_x=$(jq -r --arg title "$second" '.[] | select(.title == $title) | .x' <<< "$windows")
    [[ -n $first_x && -n $second_x ]] && ((second_x - first_x == expected)) && return 0
    sleep 0.1
  done
  echo "$message: $windows"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"

[layout.scrolling]
default_extent_fraction = 0.5

[output."HEADLESS-1"]
workspaces = 4

[[window_rule]]
match.app_id = "^named-move-owner$"
default_scrolling_column = "move-source"
default_scrolling_extent = 0.4

[[window_rule]]
match.title = "^named-move-late$"
default_scrolling_column = "move-target"
default_workspace = 2

[[window_rule]]
match.title = "^named-move-after$"
default_workspace = 2

[[window_rule]]
match.app_id = "^named-max-move-owner$"
default_scrolling_column = "max-move-source"
default_maximize = true

[[window_rule]]
match.title = "^named-max-move-late$"
default_scrolling_column = "max-move-target"
default_workspace = 3

[[window_rule]]
match.title = "^named-max-move-after$"
default_workspace = 3

[[window_rule]]
match.title = "^named-max-reclass-owner$"
default_scrolling_column = "max-reclass-source"
default_workspace = 4

[[window_rule]]
match.app_id = "^named-max-reclass-joiner$"
default_scrolling_column = "max-reclass-source"
default_workspace = 4
default_maximize = true

[[window_rule]]
match.title = "^named-max-reclass-late$"
default_scrolling_column = "max-reclass-target"

[[window_rule]]
match.title = "^named-max-reclass-after$"
default_workspace = 4

[[window_rule]]
match.app_id = "^named-owner-width-owner$"
default_scrolling_column = "owner-width-stack"

[[window_rule]]
match.title = "^named-owner-width-peer$"
default_scrolling_column = "owner-width-stack"

[[window_rule]]
match.title = "^named-owner-width-late$"
default_scrolling_column_order = 10
default_scrolling_extent = 0.7
EOF
"$UMBRIEL" msg config-reload > /dev/null

# A focused late move keeps focus and seeds the target column with the width
# already resolved from the app ID.
mkfifo "$MOVE_FIFO"
exec {move_fd}<>"$MOVE_FIFO"
APP_ID=named-move-owner TITLE_AFTER_MAP=named-move-late \
  "$CLIENT" named-move-placeholder 800 600 <&"$move_fd" > "$UMBRIEL_RUNTIME_DIR/named-move-owner.log" 2>&1 &
wait_for_windows 1
printf 'u' >&"$move_fd"
wait_for_query \
  'any(.[]; .title == "named-move-late" and (.workspace | endswith(":2")) and .active and .focused)' \
  "late named member did not retain focus on workspace 2"
spawn_client named-move-after
wait_for_windows 2
wait_for_delta named-move-late named-move-after 509 \
  "late workspace placement lost the first member's 0.4 width rule"

# Standard maximize is column state, so it must be restored after a late move.
mkfifo "$MAX_MOVE_FIFO"
exec {max_move_fd}<>"$MAX_MOVE_FIFO"
APP_ID=named-max-move-owner TITLE_AFTER_MAP=named-max-move-late \
  "$CLIENT" named-max-move-placeholder 800 600 <&"$max_move_fd" \
  > "$UMBRIEL_RUNTIME_DIR/named-max-move-owner.log" 2>&1 &
wait_for_windows 3
printf 'u' >&"$max_move_fd"
wait_for_query 'any(.[]; .title == "named-max-move-late" and (.workspace | endswith(":3")))' \
  "late maximized member did not move to workspace 3"
spawn_client named-max-move-after
wait_for_windows 4
wait_for_delta named-max-move-late named-max-move-after 1272 \
  "late workspace placement lost the first member's maximized column state"

# Reclassification within one workspace must carry that same full-width state
# into the new named column.
spawn_client named-max-reclass-owner
wait_for_windows 5
mkfifo "$MAX_RECLASSIFY_FIFO"
exec {max_reclassify_fd}<>"$MAX_RECLASSIFY_FIFO"
APP_ID=named-max-reclass-joiner TITLE_AFTER_MAP=named-max-reclass-late \
  "$CLIENT" named-max-reclass-placeholder 800 600 <&"$max_reclassify_fd" \
  > "$UMBRIEL_RUNTIME_DIR/named-max-reclassify-joiner.log" 2>&1 &
wait_for_windows 6
printf 'u' >&"$max_reclassify_fd"
wait_for_query 'any(.[]; .title == "named-max-reclass-late" and (.workspace | endswith(":4")))' \
  "maximized reclassified member did not remain on workspace 4"
spawn_client named-max-reclass-after
wait_for_windows 7
wait_for_delta named-max-reclass-late named-max-reclass-after 1272 \
  "late group reclassification lost the member's maximized column state"

# A joiner's late width is ignored, but the member that created a column still
# owns it if its title settles after another member joined.
mkfifo "$OWNER_WIDTH_FIFO"
exec {owner_width_fd}<>"$OWNER_WIDTH_FIFO"
APP_ID=named-owner-width-owner TITLE_AFTER_MAP=named-owner-width-late \
  "$CLIENT" named-owner-width-placeholder 800 600 <&"$owner_width_fd" \
  > "$UMBRIEL_RUNTIME_DIR/named-owner-width-owner.log" 2>&1 &
wait_for_windows 8
spawn_client named-owner-width-peer
wait_for_windows 9
spawn_client named-owner-width-after
wait_for_windows 10
printf 'u' >&"$owner_width_fd"
wait_for_delta named-owner-width-late named-owner-width-after 890 \
  "late width rule from the column owner did not resize its named column"

echo "late named-column lifecycle preserved ownership, focus, width, and maximize state"
