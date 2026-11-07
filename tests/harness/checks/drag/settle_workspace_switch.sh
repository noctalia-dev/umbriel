#!/usr/bin/env bash
# harness: outputs=1
# A window that still deforms must not outlive the workspace it lives in. The
# drag physics keeps a released sheet displaced past the drop, and the deferred
# re-home leaves the view under the shared drag tree — outside every workspace
# layer, so the workspace's slide offset and clip never reach it: the window
# hangs at its old screen position after its workspace has gone. It must travel
# (and hide) with its workspace, come back with it, and do the same under a
# gesture, whose trees move before the outgoing workspace is deactivated.
#
# A window still held by the cursor is the drag's payload: switching workspace
# must not steal its presentation from the drag.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly TITLE=stuck-sheet
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/switch.png"
readonly REGION="${OUTPUT_W}x${OUTPUT_H}+0+0"
# A quarter of the 400x300 sheet: proof the window is drawn, not a stray edge.
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
# A slow spring keeps the sheet displaced far past the workspace slide, so every
# sample below lands while the deform is still running.
[animation.windows_drag]
physics = true
stiffness = 1.0
damping = 1.0
max_displacement_px = 200
# A linear slide makes duration_ms meaningful (the default curve is a spring and
# ignores it) and puts the halfway sample at half the travel.
[animation.workspaces]
enabled = true
duration_ms = 300
curve = "linear"

# Static workspaces: a dynamic inventory would prune the emptied workspace and
# move the window instead of leaving it where the check put it.
[output.HEADLESS-1]
position = [0, 0]
workspaces = ["ONE", "TWO"]

[[window_rule]]
match.title = "^stuck-sheet$"
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

box() { "$UMBRIEL" windows --json | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | "\(.x) \(.y) \(.w) \(.h)"'; }
home() { "$UMBRIEL" windows --json | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | .workspace'; }
# The window reports the canonical "<output>:<serial>" id; the assertions speak names.
home_name() {
  local id
  id=$(home)
  "$UMBRIEL" workspaces --json | jq -r --arg id "$id" '.[] | select(.id == $id) | .name'
}
active() { "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name'; }
capture() { grim -o HEADLESS-1 "$IMAGE"; }
green() { capture; "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.8 && r < 0.2 && b < 0.2'; }
bbox() { capture; "$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'g > 0.8 && r < 0.2 && b < 0.2'; }

# The drawn sheet must have travelled with its workspace: still drawn, and moved
# along whichever axis this output arranges workspaces on by a large share of the
# slide, or clipped at the edge it left through. A view parked under the shared
# drag tree keeps drawing at its own coordinates.
assert_travelled() { # $1 label, $2 box "x y w h", $3 drawn bbox, $4 minimum shift
  local label=$1 bxx bxy bxw bxh drx dry drw drh
  read -r bxx bxy bxw bxh <<< "$2"
  read -r drx dry drw drh <<< "$3"
  local dx=$((drx - bxx)) dy=$((dry - bxy))
  ((dx < 0)) && dx=$((-dx))
  ((dy < 0)) && dy=$((-dy))
  local moved=$((dx > dy ? dx : dy))
  if ((moved >= $4)); then
    return 0
  fi
  if (((drx == 0 && bxx >= $4) || (dry == 0 && bxy >= $4))); then
    return 0
  fi
  echo "$label: the deforming window did not travel with its workspace (drawn at ${drx},${dry}, box ${bxx},${bxy})"
  exit 1
}

# The frame must keep moving on ordered ticks while the spring rings: every other
# animation in this config is 1 ms, so a static window means the deform never
# engaged and the deferral under test was never taken.
require_deforming() {
  local label=$1 _ changed
  capture
  cp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/deform-before.png"
  "$UMBRIEL" clock-advance 200 > /dev/null
  capture
  read -r _ changed < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/deform-before.png" diff "$IMAGE" "$REGION")
  if ((changed == 0)); then
    echo "$label: the released sheet is not deforming (no pixel moved in 200 ms), the wait was never taken"
    exit 1
  fi
}

# Drags the window by 120 px and releases it, leaving the spring ringing.
drag_and_release() {
  local wx wy ww wh
  read -r wx wy ww wh < <(box)
  local gx=$((wx + ww / 2)) gy=$((wy + wh / 2))
  pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
    move "$gx" "$gy" mod super press "$BTN_LEFT" \
    move $((gx + 40)) "$gy" move $((gx + 80)) "$gy" move $((gx + 120)) "$gy" \
    -- release "$BTN_LEFT" mod none
}

# Grabs the window and holds it, leaving the sheet displaced under the cursor.
grab_and_hold() {
  local wx wy ww wh
  read -r wx wy ww wh < <(box)
  local gx=$((wx + ww / 2)) gy=$((wy + wh / 2))
  pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
    move "$gx" "$gy" mod super press "$BTN_LEFT" \
    move $((gx + 40)) "$gy" move $((gx + 80)) "$gy" move $((gx + 120)) "$gy"
}

read -r wx wy ww wh < <(box)
# The drawn resting extent is the baseline the settled deform must return to;
# the client's surface and the presented box are not the same rectangle.
read -r bx by bw bh < <(bbox)
if ((bw < ww / 2 || bh < wh / 2)); then
  echo "scenario never engaged: the window draws ${bw}x${bh} in a ${ww}x${wh} box"
  exit 1
fi

# Phase 1 — a gesture switch. A swipe moves the base workspace's tree before the
# outgoing workspace is deactivated, so nothing but an explicit flush can land
# the released sheet's wait in time.
drag_and_release
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 16 > /dev/null
pre=$(green)
pointer_release
"$UMBRIEL" clock-advance 16 > /dev/null
require_deforming "gesture"
read -r wx wy ww wh < <(box)
# The active workspace axis decides which delta travels; send the same travel on
# both so the swipe works whichever axis this output arranges.
"$UMBRIEL" swipe-test begin 1000 > /dev/null
"$UMBRIEL" swipe-test update 1016 -80 -80 > /dev/null
"$UMBRIEL" clock-advance 16 > /dev/null
gesture_count=$(green)
read -r gx gy gw gh < <(bbox)
"$UMBRIEL" swipe-test end 1032 1 > /dev/null
"$UMBRIEL" clock-advance 600 > /dev/null
"$UMBRIEL" clock-resume
"$UMBRIEL" settle > /dev/null

if ((pre < FLOOR)); then
  echo "scenario never engaged: the deformed sheet does not draw before the swipe (green=$pre)"
  exit 1
fi
if ((gesture_count < FLOOR)); then
  echo "the deforming window left the slide instead of travelling with it (green=$gesture_count)"
  exit 1
fi
assert_travelled "into a gesture" "$wx $wy $ww $wh" "$gx $gy $gw $gh" 120

# Phase 2 — an activate switch. The window keeps living on ONE: it must slide out
# with it, not hang at its old spot.
drag_and_release
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 16 > /dev/null
held=$(green)
pointer_release
"$UMBRIEL" clock-advance 16 > /dev/null
require_deforming "switch"

if [[ $(home_name) != ONE || $(active) != ONE ]]; then
  echo "scenario never engaged: window on '$(home_name)', active '$(active)', want ONE/ONE"
  exit 1
fi
if ((held < FLOOR)); then
  echo "scenario never engaged: the deformed sheet does not draw before the switch (green=$held)"
  exit 1
fi
read -r wx wy ww wh < <(box)

"$UMBRIEL" msg workspace-switch TWO > /dev/null
"$UMBRIEL" clock-advance 60 > /dev/null
mid=$(green)
read -r mx my mw mh < <(bbox)
"$UMBRIEL" clock-advance 400 > /dev/null
away=$(green)
active_after=$(active)

if [[ $active_after != TWO ]]; then
  echo "the switch never happened: on workspace '$active_after', want TWO"
  exit 1
fi
if [[ $(home_name) != ONE ]]; then
  echo "the release moved the window off its workspace: '$(home_name)', want ONE"
  exit 1
fi
if ((mid < FLOOR)); then
  echo "the deforming window left the slide instead of travelling with it (green=$mid halfway)"
  exit 1
fi
assert_travelled "mid-slide" "$wx $wy $ww $wh" "$mx $my $mw $mh" 120
if ((away >= FLOOR)); then
  echo "the deforming window stayed on screen after its workspace left (green=$away)"
  exit 1
fi

# Switching back presents it again, at rest on its committed box once the slow
# spring has run out; the clock stays frozen so that settle is ordered here.
"$UMBRIEL" msg workspace-switch ONE > /dev/null
"$UMBRIEL" clock-advance 300 > /dev/null
"$UMBRIEL" clock-advance 6000 > /dev/null
back=$(green)
read -r rx ry rw rh < <(bbox)
"$UMBRIEL" clock-resume
"$UMBRIEL" settle > /dev/null
if ((back < FLOOR)); then
  echo "the window did not come back with its workspace (green=$back)"
  exit 1
fi
if (((rw - bw) > 2 || (bw - rw) > 2 || (rh - bh) > 2 || (bh - rh) > 2)); then
  echo "the deform left a residue once it settled: drawn ${rw}x${rh}, resting ${bw}x${bh}"
  exit 1
fi

# Phase 3 — the window is still held by the cursor when the workspace switches.
# The drag owns its presentation: it must stay drawn for the whole slide, then
# land on the workspace it is dropped on.
grab_and_hold
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 16 > /dev/null
"$UMBRIEL" msg workspace-switch TWO > /dev/null
"$UMBRIEL" clock-advance 450 > /dev/null
grabbed=$(green)
"$UMBRIEL" clock-resume
pointer_release
"$UMBRIEL" settle > /dev/null
posed=$(green)
dropped=$(home_name)

if ((grabbed < FLOOR)); then
  echo "the held window vanished with the workspace it was dragged from (green=$grabbed)"
  exit 1
fi
if [[ $dropped != TWO ]]; then
  echo "the drop did not land the window on the visible workspace: '$dropped', want TWO"
  exit 1
fi
if ((posed < FLOOR)); then
  echo "the dropped window is not presented on its own workspace (green=$posed)"
  exit 1
fi
"$UMBRIEL" msg workspace-switch ONE > /dev/null
"$UMBRIEL" settle > /dev/null
left_behind=$(green)
if ((left_behind >= FLOOR)); then
  echo "the dropped window stayed on screen after its workspace left (green=$left_behind)"
  exit 1
fi
echo "a deforming window travels and hides with its workspace, and a held one stays with the cursor"
