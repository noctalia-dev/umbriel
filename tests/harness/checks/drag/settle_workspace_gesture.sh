#!/usr/bin/env bash
# harness: outputs=1
# A gesture moves the base workspace's tree before the outgoing workspace is
# deactivated, so nothing but an explicit flush can land a released sheet's
# waiting re-home in time: without it the sheet stays parked under the shared
# drag tree, outside every workspace layer, and hangs at its old screen position
# for the whole swipe. A cancelled swipe must come back with it; a committed one
# must take it off screen.
#
# This phase sits in the gesture path because the deactivation that would land
# the wait on its own only happens at commit.
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
settle_drawn_extent "$TITLE" > /dev/null

settle_drag_queue "$TITLE" "$SPAN"
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 16 > /dev/null
held=$(settle_green)
pointer_release
"$UMBRIEL" clock-advance 16 > /dev/null
settle_require_deforming "gesture"
read -r wx wy ww wh < <(settle_box "$TITLE")

# A cancelled swipe comes back to its workspace.
"$UMBRIEL" swipe-test begin 1000 > /dev/null
"$UMBRIEL" swipe-test update 1016 -80 -80 > /dev/null
"$UMBRIEL" clock-advance 16 > /dev/null
cancelled=$(settle_green)
read -r cx cy cw ch < <(settle_bbox)
"$UMBRIEL" swipe-test end 1032 1 > /dev/null
"$UMBRIEL" clock-advance 600 > /dev/null
cancelled_after=$(settle_green)
cancelled_workspace=$(settle_active_workspace)

# Carry the step past the halfway point so the next release commits it.
"$UMBRIEL" swipe-test begin 2000 > /dev/null
"$UMBRIEL" swipe-test update 2016 -80 -80 > /dev/null
"$UMBRIEL" clock-advance 16 > /dev/null
committed=$(settle_green)
read -r mx my mw mh < <(settle_bbox)
"$UMBRIEL" swipe-test update 2032 -220 -220 > /dev/null
"$UMBRIEL" swipe-test end 2048 0 > /dev/null
"$UMBRIEL" clock-advance 800 > /dev/null
committed_away=$(settle_green)
committed_workspace=$(settle_active_workspace)
"$UMBRIEL" clock-resume
"$UMBRIEL" settle > /dev/null

if ((held < SETTLE_FLOOR)); then
  echo "scenario never engaged: the deformed sheet does not draw before the swipe (green=$held)"
  exit 1
fi
if ((cancelled < SETTLE_FLOOR)); then
  echo "the deforming window left the cancelled slide instead of travelling with it (green=$cancelled)"
  exit 1
fi
settle_assert_travelled "into a cancelled swipe" "$wx $wy $ww $wh" "$cx $cy $cw $ch" 120
if [[ $cancelled_workspace != ONE || $cancelled_after -lt SETTLE_FLOOR ]]; then
  echo "a cancelled swipe did not leave the window on its workspace (workspace $cancelled_workspace, green=$cancelled_after)"
  exit 1
fi
if ((committed < SETTLE_FLOOR)); then
  echo "the window left the committed slide instead of travelling with it (green=$committed)"
  exit 1
fi
settle_assert_travelled "into a committed swipe" "$wx $wy $ww $wh" "$mx $my $mw $mh" 120
if [[ $committed_workspace != TWO ]]; then
  echo "the committed swipe did not switch workspace: on '$committed_workspace', want TWO"
  exit 1
fi
if ((committed_away >= SETTLE_FLOOR)); then
  echo "the window stayed on screen after its workspace left (green=$committed_away)"
  exit 1
fi
echo "a deforming window travels and hides with a gesture, cancelled or committed"
