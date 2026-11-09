#!/usr/bin/env bash
# harness: outputs=1
# A window that still deforms must not outlive the workspace it lives in. The
# drag physics keeps a released sheet displaced past the drop and the deferred
# re-home leaves it under the shared drag tree, outside every workspace layer, so
# the workspace's slide offset and clip never reach it: it hangs at its old screen
# position after its workspace has gone. It must slide out with the workspace,
# come back with it, and leave no residue once the slow spring has run out.
#
# The switch is an activate (not a gesture), so the workspace is deactivated at
# once and the flight is ordered on the frozen clock.
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
read -r bw bh < <(settle_drawn_extent "$TITLE")

settle_drag_queue "$TITLE" "$SPAN"
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 16 > /dev/null
held=$(settle_green)
pointer_release
"$UMBRIEL" clock-advance 16 > /dev/null
settle_require_deforming "switch"

if [[ $(settle_workspace_name "$(settle_workspace "$TITLE")") != ONE || $(settle_active_workspace) != ONE ]]; then
  echo "scenario never engaged: window and active workspace want ONE/ONE"
  exit 1
fi
if ((held < SETTLE_FLOOR)); then
  echo "scenario never engaged: the deformed sheet does not draw before the switch (green=$held)"
  exit 1
fi
read -r wx wy ww wh < <(settle_box "$TITLE")

read -r px py _ _ < <(settle_bbox)
"$UMBRIEL" msg workspace-switch TWO > /dev/null
# The switch is dispatched asynchronously, so the slide can begin after the next
# ordered tick: advance until the sheet has travelled. This output arranges its
# workspaces vertically (the default axis, no `workspace_axis` override) while the
# drag displaced the sheet horizontally, so vertical motion here is the slide.
advanced=0
while :; do
  "$UMBRIEL" clock-advance 16 > /dev/null
  ((advanced += 16))
  read -r _ ny _ _ < <(settle_bbox)
  dy=$((ny - py))
  ((dy < 0)) && dy=$((-dy))
  ((dy >= 120)) && break
  if ((advanced >= 240)); then
    echo "the deforming window did not travel with its workspace: still drawn at y=$ny (was $py) through the whole slide"
    exit 1
  fi
done
mid=$(settle_green)
read -r mx my mw mh < <(settle_bbox)
"$UMBRIEL" clock-advance 400 > /dev/null
away=$(settle_green)
active_after=$(settle_active_workspace)

if [[ $active_after != TWO ]]; then
  echo "the switch never happened: on workspace '$active_after', want TWO"
  exit 1
fi
if ((mid < SETTLE_FLOOR)); then
  echo "the deforming window left the slide instead of travelling with it (green=$mid halfway)"
  exit 1
fi
settle_assert_travelled "mid-slide" "$wx $wy $ww $wh" "$mx $my $mw $mh" 120
if ((away >= SETTLE_FLOOR)); then
  echo "the deforming window stayed on screen after its workspace left (green=$away)"
  exit 1
fi

# Switching back presents it again at rest on its committed box once the slow
# spring has run out; the clock stays frozen so that settle is ordered here.
"$UMBRIEL" msg workspace-switch ONE > /dev/null
"$UMBRIEL" clock-advance 300 > /dev/null
"$UMBRIEL" clock-advance 6000 > /dev/null
back=$(settle_green)
read -r _ _ rw rh < <(settle_bbox)
"$UMBRIEL" clock-resume
"$UMBRIEL" settle > /dev/null

if [[ $(settle_workspace_name "$(settle_workspace "$TITLE")") != ONE ]]; then
  echo "the release moved the window off its workspace, want ONE"
  exit 1
fi
if ((back < SETTLE_FLOOR)); then
  echo "the window did not come back with its workspace (green=$back)"
  exit 1
fi
if (((rw - bw) > 2 || (bw - rw) > 2 || (rh - bh) > 2 || (bh - rh) > 2)); then
  echo "the deform left a residue once it settled: drawn ${rw}x${rh}, resting ${bw}x${bh}"
  exit 1
fi
echo "a deforming window slides out and back with its workspace and settles clean"
