#!/usr/bin/env bash
# harness: outputs=2
# Moving a workspace to another output while one of its windows still deforms
# must land the released sheet's deferred re-home first. The sheet is parked
# under the shared drag tree — outside every workspace tree — so a transfer that
# travels the trees without landing it leaves the sheet drawn at its old SCREEN
# position on the source output, and rewrites its remembered position in the old
# output's coordinate space, so the settle lands it a second time somewhere
# else. The sheet must leave with its workspace and appear on the destination.
#
# The transfer also deactivates the workspace, which lands the wait on its own —
# this check pins that ordering as much as the explicit flush before the trees
# travel: a path that defers the deactivation (as gestures do) would break it.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly TITLE=transfer-sheet
readonly SHOT="$UMBRIEL_RUNTIME_DIR/transfer"
readonly FLOOR=20000

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "floating"

[input.focus]
follows_mouse = false

[colors]
backdrop = "#000000FF"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
drag_opacity = 1.0
[appearance.shadow]
enabled = false
[animation]
duration_ms = 1
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
# A slow spring keeps the sheet displaced far past the transfer, so every sample
# below lands while the deform is still running.
[animation.windows_drag]
physics = true
stiffness = 1.0
damping = 1.0
max_displacement_px = 200

# Static workspaces with names unique per output: the transfer moves the named
# workspace, and a dynamic inventory would renumber it out from under the
# assertion.
[output.HEADLESS-1]
position = [0, 0]
workspaces = ["ONE", "TWO"]
[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["THREE", "FOUR"]

[[window_rule]]
match.title = "^transfer-sheet$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

FILL_COLOR=0xFF00FF00 "$UMBRIEL_UNMAP_CLIENT" "$TITLE" 400 300 > "$UMBRIEL_RUNTIME_DIR/$TITLE.log" 2>&1 &
for _ in $(seq 80); do
  "$UMBRIEL" windows --json | jq -e --arg title "$TITLE" 'any(.[]; .title == $title)' > /dev/null && break
  sleep 0.025
done
"$UMBRIEL" settle > /dev/null

workspace_id() { "$UMBRIEL" windows --json | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | .workspace'; }
box() { "$UMBRIEL" windows --json | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | "\(.x) \(.y) \(.w) \(.h)"'; }
green_on() {
  grim -o "$1" "$SHOT-$1.png"
  "$UMBRIEL_PIXEL_PROBE" "$SHOT-$1.png" count 'g > 0.8 && r < 0.2 && b < 0.2'
}
require_deforming() {
  local label=$1 _ changed
  green_on "$home_out" > /dev/null
  cp "$SHOT-$home_out.png" "$SHOT-deform-before.png"
  "$UMBRIEL" clock-advance 200 > /dev/null
  green_on "$home_out" > /dev/null
  read -r _ changed < <("$UMBRIEL_PIXEL_PROBE" "$SHOT-deform-before.png" diff "$SHOT-$home_out.png" "${OUTPUT_W}x${OUTPUT_H}+0+0")
  if ((changed == 0)); then
    echo "$label: the released sheet is not deforming (no pixel moved in 200 ms), the wait was never taken"
    exit 1
  fi
}

ws_id=$(workspace_id)
home_out=${ws_id%%:*}
if [[ $home_out == HEADLESS-1 ]]; then dest_out=HEADLESS-2; else dest_out=HEADLESS-1; fi
ws_name=$("$UMBRIEL" workspaces --json | jq -r --arg id "$ws_id" '.[] | select(.id == $id) | .name')
if [[ -z $ws_name ]]; then
  echo "scenario never engaged: the window reports workspace '$ws_id' with no name"
  exit 1
fi

pre_home=$(green_on "$home_out")
pre_dest=$(green_on "$dest_out")
if ((pre_home < FLOOR)); then
  echo "scenario never engaged: the window does not draw on $home_out (green=$pre_home)"
  exit 1
fi
if ((pre_dest >= FLOOR)); then
  echo "scenario never engaged: the window already draws on $dest_out before the transfer (green=$pre_dest)"
  exit 1
fi

# `windows` reports screen coordinates, so the drag point needs no output offset.
read -r wx wy ww wh < <(box)
gx=$((wx + ww / 2))
gy=$((wy + wh / 2))
pointer_hold 2560 720 \
  move "$gx" "$gy" mod super press "$BTN_LEFT" \
  move $((gx + 40)) "$gy" move $((gx + 80)) "$gy" move $((gx + 120)) "$gy" \
  -- release "$BTN_LEFT" mod none
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 16 > /dev/null
settling_home=$(green_on "$home_out")
"$UMBRIEL" clock-advance 16 > /dev/null
require_deforming "transfer"
"$UMBRIEL" clock-advance 16 > /dev/null

# The transfer is the operation under test: it reparents the workspace trees.
"$UMBRIEL" msg workspace-move-to-output "$dest_out" "$ws_name" > /dev/null
"$UMBRIEL" clock-advance 60 > /dev/null
moved_home=$(green_on "$home_out")
moved_dest=$(green_on "$dest_out")
"$UMBRIEL" clock-resume
"$UMBRIEL" settle > /dev/null
landed_home=$(green_on "$home_out")
landed_dest=$(green_on "$dest_out")

if ((settling_home < FLOOR)); then
  echo "scenario never engaged: the released sheet does not draw on $home_out before the transfer (green=$settling_home)"
  exit 1
fi
if ((moved_dest < FLOOR)); then
  echo "the transferred sheet did not appear on $dest_out with its workspace (green=$moved_dest)"
  exit 1
fi
if ((moved_home >= FLOOR)); then
  echo "the settling window stayed on $home_out after its workspace moved (green=$moved_home)"
  exit 1
fi
if ((landed_home >= FLOOR)); then
  echo "the window came back to $home_out once the deform settled (green=$landed_home)"
  exit 1
fi
if ((landed_dest < FLOOR)); then
  echo "the sheet left $dest_out once the deform settled (green=$landed_dest)"
  exit 1
fi
echo "a settling window moves outputs with its workspace, and stays where it landed"
