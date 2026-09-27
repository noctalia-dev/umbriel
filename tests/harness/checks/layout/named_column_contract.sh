#!/usr/bin/env bash
# Named scrolling columns honor orientation and workspace scope without changing other layouts.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly VERTICAL_CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly MAX_LOG="$UMBRIEL_RUNTIME_DIR/named-vertical-max.log"

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

workspace_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .workspace'
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"

[layout.scrolling]
default_extent_fraction = 0.5

# The whole output starts with horizontally arranged workspaces, so its scrolling
# strips run vertically for the named-lane phase below.
[output."HEADLESS-1"]
workspaces = 4
workspace_axis = "horizontal"

[[workspace]]
index = 3
layout.mode = "master"

[[window_rule]]
match.title = "^named-vertical-later$"
default_scrolling_column = "vertical-stack"
default_scrolling_column_order = 20
default_scrolling_extent_px = 166

[[window_rule]]
match.title = "^named-vertical-first$"
default_scrolling_column = "vertical-stack"
default_scrolling_column_order = 10
default_scrolling_extent = 0.75

[[window_rule]]
match.title = "^named-vertical-max$"
default_scrolling_column = "vertical-stack"
default_scrolling_column_order = 15
default_maximize = true

[[window_rule]]
match.title = "^named-split-(a|b|c|local)$"
default_scrolling_column = "split-stack"

[[window_rule]]
match.title = "^named-split-a$"
default_scrolling_column_order = 10

[[window_rule]]
match.title = "^named-split-b$"
default_scrolling_column_order = 20

[[window_rule]]
match.title = "^named-split-c$"
default_scrolling_column_order = 15

[[window_rule]]
match.title = "^named-master-"
default_scrolling_column = "ignored-master-stack"

[[window_rule]]
match.title = "^named-float-"
default_scrolling_column = "ignored-floating-stack"
default_floating = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

# A vertical named lane uses the first launched member's primary-axis extent,
# while configured order controls the cross-axis row positions.
"$VERTICAL_CLIENT" named-vertical-later 800 600 > "$UMBRIEL_RUNTIME_DIR/named-vertical-later.log" 2>&1 &
wait_for_windows 1
"$VERTICAL_CLIENT" named-vertical-first 800 600 > "$UMBRIEL_RUNTIME_DIR/named-vertical-first.log" 2>&1 &
wait_for_windows 2
wait_for_query \
  'all(.[]; .w == 624 and .h == 166 and .y == 277) and any(.[]; .title == "named-vertical-first" and .x == 10) and any(.[]; .title == "named-vertical-later" and .x == 646)' \
  "vertical named lane did not retain the first launched member's width or configured order"

# Maximizing a joining member expands the established lane. Its first configure
# must already match the final arranged size.
"$VERTICAL_CLIENT" named-vertical-max 800 600 > "$MAX_LOG" 2>&1 &
wait_for_windows 3
wait_for_query \
  'all(.[]; .w == 412 and .h == 700 and .y == 10) and any(.[]; .title == "named-vertical-first" and .x == 10) and any(.[]; .title == "named-vertical-max" and .x == 434) and any(.[]; .title == "named-vertical-later" and .x == 858)' \
  "maximized member did not join and expand the vertical named lane"
first_configure=$(awk '/^first-configure /{print $2 "x" $3; exit}' "$MAX_LOG")
final_size=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "named-vertical-max") | "\(.w)x\(.h)"')
if [[ $first_configure != "$final_size" ]]; then
  echo "vertical named maximize first configure $first_configure did not match final size $final_size"
  exit 1
fi
vertical_workspace=$(workspace_of named-vertical-first)

# Restore vertically arranged workspaces so the strip runs horizontally again.
sed -i 's/^workspace_axis = "horizontal"$/workspace_axis = "vertical"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

# Once users split a named column manually, new members join the first named
# column in strip order, not whichever column happens to be focused.
"$UMBRIEL" msg workspace-switch:2 > /dev/null
spawn_client named-split-a
wait_for_windows 4
spawn_client named-split-b
wait_for_windows 5
wait_for_query \
  '[.[] | select(.title == "named-split-a" or .title == "named-split-b") | .x] | unique | length == 1' \
  "horizontal named members did not initially share a column"
"$UMBRIEL" msg window-consume-or-expel-right > /dev/null
wait_for_query \
  'any(.[]; .title == "named-split-a" and .x == 10) and any(.[]; .title == "named-split-b" and .x > 600)' \
  "manual expel did not split the named column"
spawn_client named-split-c
wait_for_windows 6
wait_for_query \
  'any(.[]; .title == "named-split-a" and .x == 10 and .y == 10) and any(.[]; .title == "named-split-c" and .x == 10 and .y > 10) and any(.[]; .title == "named-split-b" and .x > 600)' \
  "new member did not join the first named column after a manual split"
split_workspace=$(workspace_of named-split-a)

# The same name on another workspace starts a local column instead of reaching
# into the existing workspace's layout.
"$UMBRIEL" msg workspace-switch:4 > /dev/null
spawn_client named-split-local
wait_for_windows 7
local_workspace=$(workspace_of named-split-local)
if [[ -z $vertical_workspace || -z $split_workspace || -z $local_workspace \
      || $vertical_workspace == "$split_workspace" || $split_workspace == "$local_workspace" ]]; then
  echo "named columns were not scoped to distinct workspaces: $("$UMBRIEL" windows --json)"
  exit 1
fi

# Floating views retain floating placement even when they share a name.
spawn_client named-float-a
wait_for_windows 8
spawn_client named-float-b
wait_for_windows 9
wait_for_query \
  '[.[] | select(.title == "named-float-a" or .title == "named-float-b")] | length == 2 and all(.[]; .floating == true)' \
  "named-column rules changed floating placement"

# Master layout keeps its own insertion policy rather than treating the name as
# a scrolling column.
"$UMBRIEL" msg workspace-switch:3 > /dev/null
spawn_client named-master-a
wait_for_windows 10
spawn_client named-master-b
wait_for_windows 11
wait_for_query \
  '[.[] | select(.title == "named-master-a" or .title == "named-master-b") | .x] | unique | length == 2' \
  "named-column rules changed master layout insertion"

echo "named columns honor vertical sizing, manual splits, workspace scope, and layout applicability"
