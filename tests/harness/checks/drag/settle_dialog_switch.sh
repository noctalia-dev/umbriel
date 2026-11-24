#!/usr/bin/env bash
# harness: outputs=1
# A dragged window carries its transient dialog into the drag tree. A workspace
# switch must not disable a dialog the drag elevated with its parent: the
# workspace's own visibility pass would hide it, and the drag tree — not the
# workspace — owns it for as long as the relation is live, held or settling.
#
# That ownership ends when the sheet lands. The dialog must then follow its
# parent back into the workspace tree, so leaving the workspace takes it off
# screen; membership in the drag tree alone must not keep it presented. A drop
# that moved the parent to the visible workspace leaves the dialog on the
# workspace it rode out of, which no longer presents it.
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

# Phase 1 — the ride ends when the sheet settles. Releasing on the dialog's own
# workspace must land both home, so switching that workspace away hides the
# dialog instead of leaving it drawn under the drag tree.
"$UMBRIEL" clock-freeze
settle_drag_corner "$PARENT" "$SPAN" release
pointer_release
settle_require_deforming "dialog switch"
# The frozen clock runs the slow release spring and both slides, so the phase
# costs no real time.
"$UMBRIEL" clock-advance 6000 > /dev/null
landed=$(settle_red)
landed_workspace=$(settle_workspace_name "$(settle_workspace "$PARENT")")
"$UMBRIEL" msg workspace-switch TWO > /dev/null
"$UMBRIEL" clock-advance 450 > /dev/null
left_behind=$(settle_red)
"$UMBRIEL" msg workspace-switch ONE > /dev/null
"$UMBRIEL" clock-advance 450 > /dev/null
returned=$(settle_red)
"$UMBRIEL" clock-resume
"$UMBRIEL" settle > /dev/null

if ((landed < SETTLE_DIALOG_FLOOR)); then
  echo "scenario never engaged: the settled dialog is not drawn with its parent (red=$landed)"
  exit 1
fi
if [[ $landed_workspace != ONE ]]; then
  echo "scenario never engaged: the release moved the parent to '$landed_workspace', want ONE"
  exit 1
fi
if ((left_behind != 0)); then
  echo "the settled dialog stayed drawn after its workspace left (red=$left_behind)"
  exit 1
fi
if ((returned < SETTLE_DIALOG_FLOOR)); then
  echo "the settled dialog did not come back with its workspace (red=$returned)"
  exit 1
fi

# Phase 2 — the ride is live across a held switch.
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
after_drop=$(settle_red)

if ((held < SETTLE_DIALOG_FLOOR)); then
  echo "scenario never engaged: the dialog does not draw with its parent's drag (red=$held)"
  exit 1
fi
if ((across < SETTLE_DIALOG_FLOOR)); then
  echo "the drag's child dialog was hidden with the workspace (red=$held -> $across)"
  exit 1
fi
if ((after_drop != 0)); then
  echo "the dropped drag left its dialog drawn over the destination workspace (red=$after_drop)"
  exit 1
fi
echo "a dragged window's dialog rides along across a workspace switch"
