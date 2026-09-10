#!/usr/bin/env bash
# A modal dialog blocks the application's windows on its own workspace only, so switching to a workspace that holds
# another of them stays there.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/modal-dialog-workspace.log"

windows() {
  "$UMBRIEL" windows --json
}

field_of() {
  local title=$1 field=$2
  windows | jq -r --arg title "$title" --arg field "$field" '.[] | select(.title == $title) | .[$field]'
}

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r '[.[] | select(.active) | .name] | if length == 1 then .[0] else "none" end'
}

workspace_id() {
  local name=$1
  "$UMBRIEL" workspaces --json | jq -r --arg name "$name" '.[] | select(.name == $name) | .id'
}

wait_for_window_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $(windows)"
  return 1
}

wait_for_field() {
  local title=$1 field=$2 want=$3
  for _ in $(seq 60); do
    [[ $(field_of "$title" "$field") == "$want" ]] && return 0
    sleep 0.1
  done
  echo "expected $title $field=$want, got: $(windows)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[window_rule]]
match.title = "^transient-unrelated$"
default_workspace = 2

[[window_rule]]
match.title = "^transient-child$"
default_workspace = 1
EOF
"$UMBRIEL" msg config-reload > /dev/null

# The parent opens on workspace 1 and the application's other window on workspace 2, which becomes active. The dialog
# maps onto the active workspace, so a rule keeps it with its parent; then start from workspace 1 with it focused.
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 "$CLIENT" transient-child 400 300 > "$CLIENT_LOG" 2>&1 &
wait_for_window_count 3
wait_for_field transient-unrelated workspace "$(workspace_id 2)"
wait_for_field transient-child workspace "$(workspace_id 1)"
"$UMBRIEL" msg workspace-switch:1 > /dev/null
wait_for_field transient-child focused true
if [[ $(active_workspace) != 1 ]]; then
  echo "expected workspace 1 active, got: $(active_workspace)"
  exit 1
fi

# The window on workspace 2 is out of the dialog's reach: the switch lands there and the focus stays with it.
"$UMBRIEL" msg workspace-switch:2 > /dev/null
wait_for_field transient-unrelated focused true
sleep 0.3
if [[ $(active_workspace) != 2 ]]; then
  echo "the switch bounced back to the dialog's workspace: $(active_workspace)"
  exit 1
fi

"$UMBRIEL" msg workspace-switch:1 > /dev/null
wait_for_field transient-child focused true

echo "a modal dialog leaves the application's windows on other workspaces usable"
