#!/usr/bin/env bash
# harness: outputs=2
# Window rules preserve the TOML selector type. Integers select a position, strings select an exact name, and a late
# title can replace an integer target with a named target on another statically configured output.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"
readonly LATE_FIFO="$UMBRIEL_RUNTIME_DIR/window-rule-workspace-late-control"
readonly LATE_LOG="$UMBRIEL_RUNTIME_DIR/window-rule-workspace-late.log"

focus_left_home() {
  "$UMBRIEL" msg 'workspace-switch:"2"/HEADLESS-1' > /dev/null
}

spawn_client() {
  APP_ID="$2" "$CLIENT" "$1" 800 600 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
}

workspace_location_of() {
  local title=$1 id name
  id=$("$UMBRIEL" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | .workspace')
  [[ -n $id ]] || return 0
  name=$("$WORKSPACE" --all | awk -F'\t' -v id="$id" '$1 == id { print $2 }')
  printf '%s/%s\n' "${id%%:*}" "$name"
}

wait_for_location() {
  local title=$1 expected=$2 actual=
  for _ in $(seq 60); do
    actual=$(workspace_location_of "$title")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on $expected, got '$actual': $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"

[output.HEADLESS-1]
workspaces = ["2", "CHAT", "LEFT_LATE"]

[output.HEADLESS-2]
workspaces = ["RIGHT_HOME", "CHAT", "RIGHT_LATE"]

[[window_rule]]
match.app_id = "^numeric-name$"
default_output = "HEADLESS-1"
default_workspace = "2"

[[window_rule]]
match.app_id = "^position-two$"
default_output = "HEADLESS-1"
default_workspace = 2

[[window_rule]]
match.app_id = "^scoped-name$"
default_output = "HEADLESS-2"
default_workspace = "CHAT"

[[window_rule]]
match.app_id = "^preferred-name$"
default_workspace = "CHAT"

[[window_rule]]
match.app_id = "^unique-name$"
default_workspace = "RIGHT_LATE"

[[window_rule]]
match.app_id = "^wrong-case$"
default_output = "HEADLESS-1"
default_workspace = "right_late"

[[window_rule]]
match.app_id = "^late-target$"
default_workspace = 2

[[window_rule]]
match.title = "^late-ready$"
default_workspace = "RIGHT_LATE"
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$UMBRIEL" msg "workspace-switch:CHAT/HEADLESS-1" > /dev/null
spawn_client numeric-name numeric-name
wait_for_location numeric-name HEADLESS-1/2

focus_left_home
spawn_client position-two position-two
wait_for_location position-two HEADLESS-1/CHAT

# The duplicate CHAT name is resolved within the explicitly selected output.
focus_left_home
spawn_client scoped-name scoped-name
wait_for_location scoped-name HEADLESS-2/CHAT

# A duplicate name stays on the preferred output when no explicit output
# disambiguates it.
focus_left_home
spawn_client preferred-name preferred-name
wait_for_location preferred-name HEADLESS-1/CHAT

# With no output selector, a unique fixed-inventory owner can redirect the
# window to another output.
focus_left_home
spawn_client unique-name unique-name
wait_for_location unique-name HEADLESS-2/RIGHT_LATE

# Name matching is case-sensitive. A missing name neither creates a workspace
# nor falls back to a position.
focus_left_home
spawn_client wrong-case wrong-case
wait_for_location wrong-case HEADLESS-1/2
if "$WORKSPACE" --all | awk -F'\t' '$2 == "right_late" { found = 1 } END { exit !found }'; then
  echo "a missing exact name unexpectedly created a workspace: $("$WORKSPACE" --all)"
  exit 1
fi

# Start from the integer rule on the focused output, then settle the title. The
# new string rule must replace the selector and relocate the same mapped view.
focus_left_home
mkfifo "$LATE_FIFO"
exec {late_fd}<>"$LATE_FIFO"
APP_ID=late-target TITLE_AFTER_MAP=late-ready \
  "$CLIENT" late-placeholder 800 600 <&"$late_fd" > "$LATE_LOG" 2>&1 &
wait_for_location late-placeholder HEADLESS-1/CHAT
printf 'u' >&"$late_fd"
wait_for_location late-ready HEADLESS-2/RIGHT_LATE

echo "window rules distinguish positions from exact names and reapply a late named target"
