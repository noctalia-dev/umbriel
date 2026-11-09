#!/usr/bin/env bash
# harness: outputs=1
# A window the drag presents is the drag's payload: switching workspace must not
# steal it. The drag tree owns the window's visibility while it is held, so the
# workspace that goes away must neither hide it nor take it along — and the drop
# must land it on the workspace the user can see.
#
# This is the counterpart of settle_workspace_switch: there the sheet is released
# and must travel with its workspace, here it is held and must not.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/drag_settle_lib.sh"

readonly TITLE=stuck-sheet
readonly SPAN="${SETTLE_OUTPUT_W}x${SETTLE_OUTPUT_H}"

settle_config
settle_sheet_rule "$TITLE"
# Static workspaces: a dynamic inventory would prune the emptied workspace and
# move the window instead of leaving it where the check put it.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output.HEADLESS-1]
position = [0, 0]
workspaces = ["ONE", "TWO"]
EOF
"$UMBRIEL" msg config-reload > /dev/null

settle_map "$TITLE" 400 300 0xFF00FF00
settle_sheet_extent "$TITLE"

settle_drag_hold "$TITLE" "$SPAN"
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 16 > /dev/null
held=$(settle_green)
"$UMBRIEL" msg workspace-switch TWO > /dev/null
"$UMBRIEL" clock-advance 450 > /dev/null
across=$(settle_green)
"$UMBRIEL" clock-resume
pointer_release
"$UMBRIEL" settle > /dev/null
posed=$(settle_green)
dropped=$(settle_workspace_name "$(settle_workspace "$TITLE")")

if ((held < SETTLE_FLOOR)); then
  echo "scenario never engaged: the held window does not draw before the switch (green=$held)"
  exit 1
fi
if ((across < SETTLE_FLOOR)); then
  echo "the held window vanished with the workspace it was dragged from (green=$held -> $across)"
  exit 1
fi
if ((posed < SETTLE_FLOOR)); then
  echo "the dropped window is not presented on its own workspace (green=$posed)"
  exit 1
fi
if [[ $dropped != TWO ]]; then
  echo "the drop did not land the window on the visible workspace: '$dropped', want TWO"
  exit 1
fi

# And it belongs to that workspace now: leaving must take it off screen.
"$UMBRIEL" msg workspace-switch ONE > /dev/null
"$UMBRIEL" settle > /dev/null
left_behind=$(settle_green)
if ((left_behind >= SETTLE_FLOOR)); then
  echo "the dropped window stayed on screen after its workspace left (green=$left_behind)"
  exit 1
fi
echo "a held window stays with the cursor across a switch and lands on the visible workspace"
