#!/usr/bin/env bash
# harness: outputs=1
# The ride outlives the grab. A pinned release parks its wait on the output's
# pinned root, so the sheet settles under the drag tree while its dialog rides it,
# and every flush skips a pinned view: only the ride can keep the dialog shown.
# Switching workspace during that settle must leave both where they are.
#
# The dialog sits on its own workspace and the parent is moved onto it, so the
# switch under test leaves the dialog's workspace entirely.
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

dialog_workspace=$(settle_workspace_name "$(settle_workspace "$DIALOG")")
"$UMBRIEL" msg window-focus "$(settle_id "$PARENT")" > /dev/null
"$UMBRIEL" msg window-move-to-workspace "$dialog_workspace" > /dev/null
"$UMBRIEL" msg workspace-switch "$dialog_workspace" > /dev/null
"$UMBRIEL" settle > /dev/null
if [[ $(settle_workspace_name "$(settle_workspace "$PARENT")") != "$dialog_workspace" ]]; then
  echo "scenario never engaged: the parent did not join the dialog on '$dialog_workspace'"
  exit 1
fi
if [[ $dialog_workspace == ONE ]]; then other_workspace=TWO; else other_workspace=ONE; fi

"$UMBRIEL" msg window-focus "$(settle_id "$PARENT")" > /dev/null
"$UMBRIEL" msg window-toggle-pinned > /dev/null
"$UMBRIEL" settle > /dev/null
if [[ $(window_by_title "$PARENT" | jq -r '.pinned') != true ]]; then
  echo "scenario never engaged: the parent did not pin"
  exit 1
fi

settle_drag_corner "$PARENT" "$SPAN" release
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 16 > /dev/null
held=$(settle_red)
pointer_release
"$UMBRIEL" clock-advance 16 > /dev/null
settle_require_deforming "pinned settle"
settling=$(settle_red)
"$UMBRIEL" msg workspace-switch "$other_workspace" > /dev/null
"$UMBRIEL" clock-advance 450 > /dev/null
across=$(settle_red)
"$UMBRIEL" clock-resume
"$UMBRIEL" settle > /dev/null
landed=$(settle_red)

if ((held < SETTLE_DIALOG_FLOOR)); then
  echo "scenario never engaged: the pinned parent's drag does not carry the dialog (red=$held)"
  exit 1
fi
if ((settling < SETTLE_DIALOG_FLOOR)); then
  echo "scenario never engaged: the dialog does not draw while the pinned sheet settles (red=$settling)"
  exit 1
fi
if ((across < SETTLE_DIALOG_FLOOR)); then
  echo "the settling drag's dialog was hidden with the workspace (red=$settling -> $across)"
  exit 1
fi
if ((landed < SETTLE_DIALOG_FLOOR)); then
  echo "the dialog stayed hidden after the pinned sheet landed (red=$across -> $landed)"
  exit 1
fi
echo "a pinned sheet that settles under the drag tree keeps its dialog shown"
