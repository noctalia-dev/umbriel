#!/usr/bin/env bash
# Synthetic swipes with a deliberately divergent accelerated/unaccelerated delta pair pin the source the gesture
# pipeline consumes: travel follows the unaccelerated pair when the wlroots in use carries it (niri parity, swipe
# distance independent of finger speed) and the accelerated pair on stock wlroots, and the release lands where that
# travel projects.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

# One workspace of switch travel: gestures.cpp kSwipeWorkspacePx, niri's WORKSPACE_GESTURE_MOVEMENT.
step=300

cat > "$UMBRIEL_CONFIG" <<'CONFIG'
[general]
xwayland = false
show_cheatsheet = false
autostart = []

[animation]
enabled = false

[output."HEADLESS-1"]
workspace_axis = "vertical"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

for n in 1 2 3; do
  "$UMBRIEL" msg "workspace-switch:$n" > /dev/null
  "$UMBRIEL" settle
  "$UMBRIEL_UNMAP_CLIENT" "swipe-unaccel-$n" 400 400 > "$UMBRIEL_RUNTIME_DIR/$n.log" 2>&1 &
  await_lines "$UMBRIEL_RUNTIME_DIR/$n.log" mapped 1
done
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" settle

active_index() { "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .index'; }
[[ $(active_index) == 2 ]]

swipe() { "$UMBRIEL" swipe-test "$@" --json; }

swipe begin 0 > /dev/null
# Cross the 16 px axis lock vertically; this event's pairs are equal, so it only decides the axis.
swipe update 100 0 16 > /dev/null
# Divergent pair: the accelerated delta is 200, the unaccelerated one 80. Travel must follow the source this build
# consumes — 16 + 80 or 16 + 200 finger pixels, in steps of $step.
divergent=$(swipe update 250 0 200 0 80)
support=$(jq -r '.unaccel_support' <<< "$divergent")
progress=$(jq -r '.progress' <<< "$divergent")
expected=0.32
if [[ $support != true ]]; then
  expected=0.72
fi
magnitude=$(jq -r 'if . < 0 then -. else . end' <<< "$progress")
awk -v got="$magnitude" -v want="$expected" 'BEGIN { exit !(got > want - 0.002 && got < want + 0.002) }' || {
  echo "swipe travel followed the wrong delta source: |progress|=$magnitude, expected $expected (unaccel_support=$support)"
  exit 1
}

# The release window trims the axis-lock sample, so the projection is the raw travel and lands on the nearest
# workspace step: 0.32 of a step stays put, 0.72 crosses to the neighbour on the side the travel points to.
swipe end 280 0
target=$(awk -v p="$progress" 'BEGIN { print int(p + (p >= 0 ? 0.5 : -0.5)) }')
expected_index=$((2 + target))
for _ in $(seq 60); do
  [[ $(active_index) == "$expected_index" ]] && break
  sleep 0.05
done
[[ $(active_index) == "$expected_index" ]] || {
  echo "release landed on workspace $(active_index), expected $expected_index (progress $progress)"
  exit 1
}
