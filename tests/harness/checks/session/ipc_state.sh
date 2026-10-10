#!/usr/bin/env bash
# The window, workspace, and layer listings report the state external tools follow, and each change to that state is
# pushed to subscribers.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly EVENTS="$UMBRIEL_RUNTIME_DIR/ipc-state-events.jsonl"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/ipc-state-control"

"$UMBRIEL" subscribe windows,workspaces,layers > "$EVENTS" &

# Waits until the newest pushed event of a family satisfies a jq predicate on its data.
await_state() {
  local family=$1 predicate=$2 reason=$3
  for _ in $(seq 60); do
    if jq -e -s --arg family "$family" "[.[] | select(.event == \$family)] | last | .data | $predicate" \
      "$EVENTS" > /dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "$reason: the newest $family event is $(jq -c -s --arg family "$family" \
    '[.[] | select(.event == $family)] | last | .data' "$EVENTS" 2> /dev/null)"
  exit 1
}

window_id() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'
}

action() {
  "$UMBRIEL" msg "$@" > /dev/null
}

# A tiled parent, a tiled unrelated window, and a floating child of the parent.
TRANSIENT_SUITE=1 "$CLIENT" state-child > "$UMBRIEL_RUNTIME_DIR/ipc-state-suite.log" 2>&1 &
await_state windows 'length == 3' "the transient suite did not map"
"$UMBRIEL" settle
parent=$(window_id transient-parent)
other=$(window_id transient-unrelated)
child=$(window_id state-child)
active=$("$UMBRIEL" windows --json | jq -r '.[] | select(.active) | .id')

await_state windows "
  (.[] | select(.id == \"$child\")
    | .parent == \"$parent\" and .floating and .column == -1 and .row == -1 and .visible
      and .output == \"HEADLESS-1\" and (.fullscreen | not) and (.maximized | not) and (.pinned | not))
  and (.[] | select(.id == \"$parent\") | .parent == \"\" and .row == 0)
  and ([.[] | select(.id != \"$child\") | .column] | sort == [0, 1])
" "the initial windows lack the expected parent, placement, and state"
await_state workspaces "
  .[] | select(.focused)
    | .window_count == 3 and .focused_window == \"$active\" and (.urgent | not) and (.layout_override | not)
" "the focused workspace does not count its windows or name its focused one"

action "window-focus:$parent"
await_state workspaces ".[] | select(.focused) | .focused_window == \"$parent\"" "focusing the parent"

action window-toggle-fullscreen
await_state windows ".[] | select(.id == \"$parent\") | .fullscreen" "fullscreen did not reach the listing"
action window-toggle-fullscreen
await_state windows ".[] | select(.id == \"$parent\") | .fullscreen | not" "leaving fullscreen did not reach the listing"

action "window-focus:$other"
action window-toggle-maximize
await_state windows ".[] | select(.id == \"$other\") | .maximized and (.maximized_to_edges | not)" \
  "maximize did not reach the listing"
action window-toggle-maximize-to-edges
await_state windows ".[] | select(.id == \"$other\") | .maximized_to_edges" \
  "maximize-to-edges did not reach the listing"
action window-toggle-maximize-to-edges
action window-toggle-maximize
await_state windows ".[] | select(.id == \"$other\") | (.maximized | not) and (.maximized_to_edges | not)" \
  "restoring the window did not reach the listing"

# A pinned window stays visible on another workspace; the others are hidden with their workspace.
action "window-focus:$child"
action window-toggle-pinned
await_state windows ".[] | select(.id == \"$child\") | .pinned" "pinning did not reach the listing"
action workspace-switch:2
await_state windows "
  (.[] | select(.id == \"$child\") | .visible)
  and all(.[] | select(.id != \"$child\"); .visible | not)
" "a workspace switch did not update visibility"
await_state workspaces '.[] | select(.focused) | .index == 2 and .focused_window == ""' \
  "the empty workspace names a focused window"
action workspace-switch:1
await_state windows 'all(.[]; .visible)' "returning to the workspace did not show its windows"

# The scrolling strip is the only layout with columns to report.
action workspace-set-layout:dwindle
await_state workspaces '.[] | select(.focused) | .layout == "dwindle" and .layout_override' \
  "a runtime layout is not reported as an override"
await_state windows 'all(.[]; .column == -1 and .row == -1)' "dwindle windows report columns"
action workspace-set-layout:scrolling

# A window that asks for activation but may not take focus is urgent, and so is its workspace.
mkfifo "$CONTROL_FIFO"
exec {control_fd}<> "$CONTROL_FIFO"
APP_ID=ipc-state-urgent REMAP_ON_STDIN=1 \
  "$CLIENT" ipc-state-urgent <&"$control_fd" > "$UMBRIEL_RUNTIME_DIR/ipc-state-urgent.log" 2>&1 &
await_state windows 'length == 4' "the activation target did not map"
action "window-close:$(window_id ipc-state-urgent)"
await_lines "$UMBRIEL_RUNTIME_DIR/ipc-state-urgent.log" unmapped 1
cat >> "$UMBRIEL_CONFIG" << 'EOF'

[[window_rule]]
match.app_id = "^ipc-state-urgent$"
default_focused = false
EOF
action config-reload
printf c >&"$control_fd"
await_state windows '.[] | select(.title == "ipc-state-urgent") | .urgent' "the vetoed activation is not urgent"
urgent_workspace=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "ipc-state-urgent") | .workspace')
await_state workspaces ".[] | select(.id == \"$urgent_workspace\") | .urgent" "the urgent window's workspace is not urgent"

# An on-demand panel takes the keyboard as it maps and gives it up to a focused window.
"$LAYER_CLIENT" HEADLESS-1 40 keyboard=on-demand > "$UMBRIEL_RUNTIME_DIR/ipc-state-panel.log" 2>&1 &
panel_pid=$!
await_state layers "
  length == 1 and (.[0]
    | .mapped and .layer == \"top\" and .output == \"HEADLESS-1\" and .pid == $panel_pid
      and .keyboard_interactivity == \"on_demand\" and .focused and .exclusive_zone == 40
      and .anchor == [\"top\", \"left\", \"right\"] and .margin == {top: 0, right: 0, bottom: 0, left: 0}
      and .x == 0 and .y == 0 and .w > 0 and .h == 40)
" "the mapped panel has unexpected layer state"
action "window-focus:$parent"
await_state layers 'length == 1 and (.[0].focused | not)' "the panel kept the keyboard after a window took focus"
kill "$panel_pid"
await_state layers 'length == 0' "the destroyed panel stayed in the layer list"

echo "window, workspace, and layer state is listed and pushed to subscribers"
