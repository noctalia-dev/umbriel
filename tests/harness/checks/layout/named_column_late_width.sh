#!/usr/bin/env bash
# Late title rules preserve named scrolling-column width ownership and may reclassify a member.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/named-width-control"
readonly RECLASSIFY_FIFO="$UMBRIEL_RUNTIME_DIR/named-reclassify-control"
readonly ORDER_FIFO="$UMBRIEL_RUNTIME_DIR/named-order-control"

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

column_delta() {
  "$UMBRIEL" windows --json | jq \
    '([.[] | select(.title == "named-width-unrelated") | .x][0])
      - ([.[] | select(.title == "named-width-owner") | .x][0])'
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"

[layout.scrolling]
default_extent_fraction = 0.5

[[window_rule]]
match.title = "^named-width-owner$"
default_scrolling_column = "width-stack"
default_scrolling_column_order = 10
default_scrolling_extent = 0.25

[[window_rule]]
match.app_id = "^named-width-joiner$"
default_scrolling_column = "width-stack"
default_scrolling_column_order = 20

[[window_rule]]
match.title = "^named-width-late$"
default_scrolling_extent = 0.9

[[window_rule]]
match.title = "^named-reclass-owner$"
default_scrolling_column = "old-stack"

[[window_rule]]
match.app_id = "^named-reclass-joiner$"
default_scrolling_column = "old-stack"
default_scrolling_extent = 0.4

[[window_rule]]
match.title = "^named-reclass-late$"
default_scrolling_column = "new-stack"

[[window_rule]]
match.title = "^named-reclass-follower$"
default_scrolling_column = "new-stack"

[[window_rule]]
match.app_id = "^named-order-joiner$"
default_scrolling_column = "order-stable"
default_workspace = 2

[[window_rule]]
match.title = "^named-order-late$"
default_scrolling_column_order = 10
default_scrolling_extent = 0.9

[[window_rule]]
match.title = "^named-order-owner$"
default_scrolling_column = "order-stable"
default_workspace = 2

[[window_rule]]
match.title = "^named-order-after$"
default_workspace = 2
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client named-width-owner
wait_for_windows 1
spawn_client named-width-unrelated
wait_for_windows 2

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
APP_ID=named-width-joiner TITLE_AFTER_MAP=named-width-late \
  "$CLIENT" named-width-placeholder 800 600 <&"$control_fd" > "$UMBRIEL_RUNTIME_DIR/named-width-joiner.log" 2>&1 &
wait_for_windows 3

for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  owner_x=$(jq -r '.[] | select(.title == "named-width-owner") | .x' <<< "$windows")
  joiner_x=$(jq -r '.[] | select(.title == "named-width-placeholder") | .x' <<< "$windows")
  [[ -n $owner_x && $owner_x == "$joiner_x" && $(column_delta) -eq 318 ]] && break
  sleep 0.1
done
before=$(column_delta)
if [[ $owner_x != "$joiner_x" || $before -ne 318 ]]; then
  echo "preselected client did not join the owner's 0.25 column: $windows"
  exit 1
fi

printf 'u' >&"$control_fd"
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  title=$(jq -r '.[] | select(.title == "named-width-late") | .title' <<< "$windows")
  [[ $title == named-width-late ]] && break
  sleep 0.1
done
after=$(column_delta)
if [[ $after -ne $before ]]; then
  echo "late member width resized its established named column: before=$before after=$after windows=$windows"
  exit 1
fi

# Reclassifying a member from one named group to a new group must split it out
# of the old stack. Later members of the new group then join its new column.
spawn_client named-reclass-owner
wait_for_windows 4
mkfifo "$RECLASSIFY_FIFO"
exec {reclassify_fd}<>"$RECLASSIFY_FIFO"
APP_ID=named-reclass-joiner TITLE_AFTER_MAP=named-reclass-late \
  "$CLIENT" named-reclass-placeholder 800 600 <&"$reclassify_fd" \
  > "$UMBRIEL_RUNTIME_DIR/named-reclass-joiner.log" 2>&1 &
wait_for_windows 5
printf 'u' >&"$reclassify_fd"

for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  old_x=$(jq -r '.[] | select(.title == "named-reclass-owner") | .x' <<< "$windows")
  new_x=$(jq -r '.[] | select(.title == "named-reclass-late") | .x' <<< "$windows")
  [[ -n $old_x && -n $new_x && $old_x != "$new_x" ]] && break
  sleep 0.1
done
if [[ -z $old_x || -z $new_x || $old_x == "$new_x" ]]; then
  echo "late group change left the member in its old named column: $windows"
  exit 1
fi

spawn_client named-reclass-follower
wait_for_windows 6
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  old_x=$(jq -r '.[] | select(.title == "named-reclass-owner") | .x' <<< "$windows")
  new_x=$(jq -r '.[] | select(.title == "named-reclass-late") | .x' <<< "$windows")
  follower_x=$(jq -r '.[] | select(.title == "named-reclass-follower") | .x' <<< "$windows")
  [[ -n $follower_x && $new_x == "$follower_x" && $old_x != "$new_x" ]] && break
  sleep 0.1
done
if [[ -z $follower_x || $new_x != "$follower_x" || $old_x == "$new_x" ]]; then
  echo "new named-group member did not join the reclassified column: $windows"
  exit 1
fi

spawn_client named-reclass-after
wait_for_windows 7
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  new_x=$(jq -r '.[] | select(.title == "named-reclass-late") | .x' <<< "$windows")
  after_x=$(jq -r '.[] | select(.title == "named-reclass-after") | .x' <<< "$windows")
  [[ -n $new_x && -n $after_x ]] && ((after_x - new_x == 509)) && break
  sleep 0.1
done
if ((after_x - new_x != 509)); then
  echo "reclassified first member did not retain its 0.4 width rule: $windows"
  exit 1
fi

# A late order-only rule must not undo a manual split from an existing named
# column or let the later-joining member apply its own width.
spawn_client named-order-owner
wait_for_windows 8
mkfifo "$ORDER_FIFO"
exec {order_fd}<>"$ORDER_FIFO"
APP_ID=named-order-joiner TITLE_AFTER_MAP=named-order-late \
  "$CLIENT" named-order-placeholder 800 600 <&"$order_fd" > "$UMBRIEL_RUNTIME_DIR/named-order-joiner.log" 2>&1 &
wait_for_windows 9
order_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "named-order-placeholder") | .id')
"$UMBRIEL" msg "window-focus:$order_id" > /dev/null
"$UMBRIEL" msg window-consume-or-expel-right > /dev/null
spawn_client named-order-after
wait_for_windows 10
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  owner_x=$(jq -r '.[] | select(.title == "named-order-owner") | .x' <<< "$windows")
  order_x=$(jq -r '.[] | select(.title == "named-order-placeholder") | .x' <<< "$windows")
  after_x=$(jq -r '.[] | select(.title == "named-order-after") | .x' <<< "$windows")
  [[ -n $owner_x && -n $order_x && -n $after_x && $owner_x != "$order_x" ]] \
    && ((after_x - order_x == 636)) && break
  sleep 0.1
done
if [[ -z $owner_x || -z $order_x || -z $after_x || $owner_x == "$order_x" ]] \
    || ((after_x - order_x != 636)); then
  echo "could not prepare the manual named-column split: $windows"
  exit 1
fi

printf 'u' >&"$order_fd"
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  owner_x=$(jq -r '.[] | select(.title == "named-order-owner") | .x' <<< "$windows")
  order_x=$(jq -r '.[] | select(.title == "named-order-late") | .x' <<< "$windows")
  after_x=$(jq -r '.[] | select(.title == "named-order-after") | .x' <<< "$windows")
  [[ -n $order_x && $owner_x != "$order_x" ]] && ((after_x - order_x == 636)) && break
  sleep 0.1
done
if [[ -z $order_x || $owner_x == "$order_x" ]] || ((after_x - order_x != 636)); then
  echo "late order-only rule undid a manual split or applied the joiner's width: $windows"
  exit 1
fi

echo "late named-column rules preserved width, grouping, and manual placement"
