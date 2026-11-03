#!/usr/bin/env bash
# A pinned window belongs to its output's active workspace: it follows every switch, never occupies a dynamic
# workspace, cannot be parked on a hidden workspace, and unpins onto the workspace on screen.
set -euo pipefail

msg() { "$UMBRIEL" msg "$@" > /dev/null; }
window() { "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'; }
active_id() { "$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .active) | .id'; }
spawn() {
  "$UMBRIEL_UNMAP_CLIENT" "$1" 300 200 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 60); do
    [[ -n $(window "$1") ]] && return
    sleep 0.05
  done
  echo "window $1 did not map"
  exit 1
}
on_active() {
  local title=$1 snapshot
  "$UMBRIEL" settle
  snapshot=$(window "$title")
  jq -e --arg id "$(active_id)" '.workspace == $id and .visible' <<< "$snapshot" > /dev/null \
    || { echo "$title is not on the active workspace $(active_id): $snapshot"; exit 1; }
}

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[[workspace]]
name = "HIDDEN"
[[window_rule]]
match.title = "^rule-pinned$"
default_pinned = true
default_workspace = "HIDDEN"
CONFIG
msg config-reload
msg workspace-switch 1
spawn tiled-one
spawn pinned
msg window-toggle-pinned
on_active pinned

# The pinned window follows each switch, and the empty trailing workspace stays the only sentinel.
msg workspace-next
on_active pinned
before=$("$UMBRIEL" workspaces --json | jq length)
msg workspace-next || true
on_active pinned
[[ $("$UMBRIEL" workspaces --json | jq length) == "$before" ]] \
  || { echo "a pinned window made the trailing sentinel occupied: $("$UMBRIEL" workspaces --json)"; exit 1; }
"$UMBRIEL" workspaces --json | jq -e --arg id "$(active_id)" \
  '.[] | select(.id == $id) | (.occupied | not) and .window_count == 0' > /dev/null

# A pinned window cannot be parked on a hidden workspace.
msg window-focus "$(window pinned | jq -r .id)"
msg window-move-to-workspace-silent 1
on_active pinned

# A rule that opens a pinned window on a hidden workspace places it on the active one.
spawn rule-pinned
on_active rule-pinned
window rule-pinned | jq -e '.pinned' > /dev/null

# Unpinning leaves the window on the workspace on screen.
msg window-focus "$(window pinned | jq -r .id)"
msg window-toggle-pinned
on_active pinned
window pinned | jq -e '.pinned | not' > /dev/null
echo "pinned windows follow the active workspace, never occupy it, and unpin where they are visible"
