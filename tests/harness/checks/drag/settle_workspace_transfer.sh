#!/usr/bin/env bash
# harness: outputs=2
# Moving a workspace to another output while one of its windows still deforms
# must land the released sheet's re-home first. The sheet is parked under the
# shared drag tree, outside every workspace tree, so a transfer that travels them
# without landing it draws the sheet at its old SCREEN position and rewrites its
# position in the old output's coordinates, so the settle lands it twice.
#
# The transfer also deactivates the workspace, which lands the wait on its own:
# this pins the user-visible result and the ordering, not the flush alone.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/drag_settle_lib.sh"

readonly TITLE=transfer-sheet
readonly SPAN="2560x$SETTLE_OUTPUT_H"

settle_config
settle_sheet_rule "$TITLE"
# Static workspaces with names unique per output: the transfer moves the named
# workspace, and a dynamic inventory would renumber it out from under the
# assertion.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output.HEADLESS-1]
position = [0, 0]
workspaces = ["ONE", "TWO"]
[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["THREE", "FOUR"]
EOF
"$UMBRIEL" msg config-reload > /dev/null

settle_map "$TITLE" 400 300 0xFF00FF00

home_out=$(settle_workspace "$TITLE")
home_out=${home_out%%:*}
if [[ $home_out == HEADLESS-1 ]]; then dest_out=HEADLESS-2; else dest_out=HEADLESS-1; fi
workspace=$(settle_workspace_name "$(settle_workspace "$TITLE")")
if [[ -z $workspace ]]; then
  echo "scenario never engaged: the window reports a workspace with no name"
  exit 1
fi

before_home=$(settle_green "$home_out")
before_dest=$(settle_green "$dest_out")
if ((before_home < SETTLE_FLOOR)); then
  echo "scenario never engaged: the window does not draw on $home_out (green=$before_home)"
  exit 1
fi
if ((before_dest >= SETTLE_FLOOR)); then
  echo "scenario never engaged: the window already draws on $dest_out before the transfer (green=$before_dest)"
  exit 1
fi

settle_drag_queue "$TITLE" "$SPAN"
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 16 > /dev/null
# The release runs on the helper's queued step, so it must happen before anything
# that asserts the released spring or the transfer: a held sheet is a different
# scenario (see settle_held_workspace_switch).
pointer_release
"$UMBRIEL" clock-advance 16 > /dev/null
settle_require_deforming "transfer" "$home_out"
settling_home=$(settle_green "$home_out")

# The transfer is the operation under test: it reparents the workspace trees.
"$UMBRIEL" msg workspace-move-to-output "$dest_out" "$workspace" > /dev/null
"$UMBRIEL" clock-advance 60 > /dev/null
moved_home=$(settle_green "$home_out")
moved_dest=$(settle_green "$dest_out")
# Run the slow spring out on the ordered clock: resuming here would spend its
# whole drain in wall time before the landing could be sampled.
"$UMBRIEL" clock-advance 6000 > /dev/null
landed_home=$(settle_green "$home_out")
landed_dest=$(settle_green "$dest_out")
"$UMBRIEL" clock-resume

if ((settling_home < SETTLE_FLOOR)); then
  echo "scenario never engaged: the released sheet does not draw on $home_out before the transfer (green=$settling_home)"
  exit 1
fi
if ((moved_dest < SETTLE_FLOOR)); then
  echo "the transferred sheet did not appear on $dest_out with its workspace (green=$moved_dest)"
  exit 1
fi
if ((moved_home >= SETTLE_FLOOR)); then
  echo "the settling window stayed on $home_out after its workspace moved (green=$moved_home)"
  exit 1
fi
if ((landed_home >= SETTLE_FLOOR)); then
  echo "the window came back to $home_out once the deform settled (green=$landed_home)"
  exit 1
fi
if ((landed_dest < SETTLE_FLOOR)); then
  echo "the sheet left $dest_out once the deform settled (green=$landed_dest)"
  exit 1
fi
echo "a settling window moves outputs with its workspace, and stays where it landed"
