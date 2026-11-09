#!/usr/bin/env bash
# harness: outputs=1
# A dragged window carries its transient dialog into the drag tree. A workspace
# switch must not disable a dialog the drag elevated with its parent: the
# workspace's own visibility pass would hide it, and the drag tree — not the
# workspace — owns it for as long as the grab lasts.
#
# The dialog is left unattached (`attach_to_parent = false`) so it keeps its own
# scene node: an attached one would ride the parent's node and hide the bug.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/drag_settle_lib.sh"

readonly PARENT=settle-parent
readonly DIALOG=settle-child
readonly SPAN="${SETTLE_OUTPUT_W}x${SETTLE_OUTPUT_H}"

settle_config
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output.HEADLESS-1]
position = [0, 0]
workspaces = ["ONE", "TWO"]

[[window_rule]]
match.title = "^settle-child$"
default_floating = true
attach_to_parent = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

settle_map_dialog "$PARENT" "$DIALOG"
settle_drag_corner "$PARENT" "$SPAN" hold
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 16 > /dev/null
held=$(settle_red)
"$UMBRIEL" msg workspace-switch TWO > /dev/null
"$UMBRIEL" clock-advance 450 > /dev/null
across=$(settle_red)
"$UMBRIEL" clock-resume
pointer_release
"$UMBRIEL" settle > /dev/null

if ((held < SETTLE_DIALOG_FLOOR)); then
  echo "scenario never engaged: the dialog does not draw with its parent's drag (red=$held)"
  exit 1
fi
if ((across < SETTLE_DIALOG_FLOOR)); then
  echo "the drag's child dialog was hidden with the workspace (red=$held -> $across)"
  exit 1
fi
echo "a dragged window's dialog rides along across a workspace switch"
