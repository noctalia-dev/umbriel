#!/usr/bin/env bash
# harness: outputs=2
# A modal dialog hangs on its parent's frame, so every way of moving the parent takes the dialog along: workspace,
# output, output unplug and replug, scratchpad, workspace moved to another output, pin, fullscreen, scrolling column
# moves, floating, a Mod-drag and an overview drop. The dialog keeps the keyboard throughout, so each action below is
# aimed at the dialog and must act on its parent.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly BTN_LEFT=272

windows() { "$UMBRIEL" windows --json; }
field() { windows | jq -r --arg title "$1" ".[] | select(.title == \$title) | .$2"; }
output_x() {
  "$UMBRIEL" outputs |
    awk -v name="$1" '$1 == name {found = 1; next} found && /Position:/ {split($2, p, ","); print p[1]; exit}'
}

# The dialog shares every placement fact of the parent, and while the parent shows it sits centred on it and holds the
# keyboard.
readonly FOLLOWS='
  (.[] | select(.title == "transient-parent")) as $p | (.[] | select(.title == "modal-dialog")) as $d |
  $d.workspace == $p.workspace and $d.output == $p.output and $d.scratchpad == $p.scratchpad
  and $d.pinned == $p.pinned and $d.visible == $p.visible
  and (($p.visible | not) or ($d.active and ($p.fullscreen or (($d.x + $d.w / 2 - $p.x - $p.w / 2 | fabs) <= 1
    and ($d.y + $d.h / 2 - $p.y - $p.h / 2 | fabs) <= 1))))'

# expect <label> <jq condition on the parent $p>
expect() {
  local label=$1 condition=$2
  for _ in $(seq 80); do
    windows | jq -e "(.[] | select(.title == \"transient-parent\")) as \$p | ($condition) and ($FOLLOWS)" \
      > /dev/null && return 0
    sleep 0.05
  done
  echo "$label: the dialog did not follow its parent: $(windows)"
  exit 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[layout]
mode = "scrolling"
EOF
"$UMBRIEL" msg config-reload > /dev/null

TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 TRANSIENT_PARENT_SIZE=640x480 "$CLIENT" modal-dialog 300 200 \
  > "$UMBRIEL_RUNTIME_DIR/modal-follow.log" 2>&1 &
for _ in $(seq 80); do
  [[ $(windows | jq 'length') == 3 ]] && break
  sleep 0.05
done
expect "opening" '($p.floating | not) and $p.column >= 0'

# The parent opens on whichever output has focus; the other one is where it gets sent.
START=$(field transient-parent output)
START_WS=$(field transient-parent workspace)
OTHER=HEADLESS-1
[[ $START == HEADLESS-1 ]] && OTHER=HEADLESS-2

# Sets TOWARD to the direction of the other output and BACK to the way home; a replugged output may change sides.
directions() {
  TOWARD=right
  BACK=left
  if (($(output_x "$OTHER") < $(output_x "$START"))); then
    TOWARD=left
    BACK=right
  fi
}

mover_workspace() {
  "$UMBRIEL" msg window-move-to-workspace 2 > /dev/null
  expect "move to workspace" "\$p.workspace == \"$START:2\""
  "$UMBRIEL" msg window-move-to-workspace 1 > /dev/null
  expect "move back to workspace" "\$p.workspace == \"$START_WS\""
}

mover_output() {
  "$UMBRIEL" msg window-move-to-output-next > /dev/null
  expect "move to output" "\$p.output == \"$OTHER\""
  "$UMBRIEL" msg window-move-to-output-next > /dev/null
  expect "move back to output" "\$p.output == \"$START\""
}

mover_unplug() {
  "$UMBRIEL" msg window-move-to-output-next > /dev/null
  expect "move to the output to unplug" "\$p.output == \"$OTHER\""
  "$UMBRIEL" output-destroy "$OTHER" > /dev/null
  expect "output unplugged" "\$p.output == \"$START\""
  "$UMBRIEL" output-create "$OTHER" > /dev/null
  expect "output replugged" "\$p.output == \"$OTHER\""
  # Output actions resolve the window from the output under the pointer, so bring the pointer to the dialog.
  "$UMBRIEL" msg window-focus-warp "$(field modal-dialog id)" > /dev/null
  "$UMBRIEL" msg window-move-to-output-next > /dev/null
  expect "back from the replugged output" "\$p.output == \"$START\""
}

mover_workspace_to_output() {
  directions
  "$UMBRIEL" msg "workspace-move-to-output-$TOWARD" > /dev/null
  expect "workspace moved to output" "\$p.output == \"$OTHER\""
  "$UMBRIEL" msg "workspace-move-to-output-$BACK" > /dev/null
  expect "workspace moved back" "\$p.output == \"$START\""
}

mover_scratchpad() {
  "$UMBRIEL" msg window-move-to-scratchpad > /dev/null
  expect "into the scratchpad" '$p.scratchpad == "default" and ($p.visible | not)'
  "$UMBRIEL" msg scratchpad-toggle > /dev/null
  expect "scratchpad shown" '$p.scratchpad == "default" and $p.visible'
  "$UMBRIEL" msg window-restore-from-scratchpad > /dev/null
  expect "out of the scratchpad" "\$p.scratchpad == \"\" and \$p.output == \"$START\" and \$p.visible"
}

mover_pin() {
  "$UMBRIEL" msg window-toggle-pinned > /dev/null
  expect "pinned" '$p.pinned'
  "$UMBRIEL" msg window-toggle-pinned > /dev/null
  expect "unpinned" '$p.pinned | not'
}

mover_fullscreen() {
  local centre_x
  centre_x=$(($(output_x "$START") + 640))
  "$UMBRIEL" msg window-toggle-fullscreen > /dev/null
  # The parent's client keeps its own buffer size, so the dialog centres on the output the parent covers.
  expect "fullscreen" "\$p.fullscreen and (.[] | select(.title == \"modal-dialog\") | .x + .w / 2 == $centre_x
    and .y + .h / 2 == 360)"
  "$UMBRIEL" msg window-toggle-fullscreen > /dev/null
  expect "windowed" '$p.fullscreen | not'
}

mover_column() {
  "$CLIENT" modal-neighbour 300 200 > "$UMBRIEL_RUNTIME_DIR/modal-neighbour.log" 2>&1 &
  for _ in $(seq 80); do
    [[ $(windows | jq 'length') == 4 ]] && break
    sleep 0.05
  done
  "$UMBRIEL" msg window-focus "$(field transient-parent id)" > /dev/null
  expect "parent beside a neighbour" "\$p.workspace == \"$START_WS\" and (\$p.floating | not)"
  local column x
  column=$(field transient-parent column)
  x=$(field transient-parent x)
  if ((column > 0)); then
    "$UMBRIEL" msg column-move-left > /dev/null
    expect "column moved left" "\$p.column == $((column - 1)) and \$p.x != $x"
    "$UMBRIEL" msg column-move-right > /dev/null
  else
    "$UMBRIEL" msg column-move-right > /dev/null
    expect "column moved right" "\$p.column == 1 and \$p.x != $x"
    "$UMBRIEL" msg column-move-left > /dev/null
  fi
  expect "column moved back" "\$p.column == $column"
}

mover_floating() {
  "$UMBRIEL" msg window-toggle-floating > /dev/null
  expect "parent floated" '$p.floating'
}

mover_drag() {
  local x y cx cy
  x=$(field transient-parent x)
  y=$(field transient-parent y)
  cx=$(windows | jq -r '.[] | select(.title == "modal-dialog") | (.x + .w / 2 | floor)')
  cy=$(windows | jq -r '.[] | select(.title == "modal-dialog") | (.y + .h / 2 | floor)')
  pointer_hold 2560 720 move "$cx" "$cy" mod logo press "$BTN_LEFT" move $((cx + 50)) $((cy + 20)) \
    move $((cx + 100)) $((cy + 40)) -- release "$BTN_LEFT" mod none
  pointer_release
  expect "Mod-drag" "\$p.x == $((x + 100)) and \$p.y == $((y + 40))"
}

mover_overview_drop() {
  local x y start_x other_x
  start_x=$(output_x "$START")
  other_x=$(output_x "$OTHER")
  x=$(($(field transient-parent x) - start_x))
  y=$(field transient-parent y)
  "$UMBRIEL" msg overview-open > /dev/null
  "$UMBRIEL" settle
  # At zoom 0.5 the active preview starts at 320,180; press the parent's card near its top left, clear of the dialog,
  # and drop it in the middle of the other output's preview.
  "$UMBRIEL_POINTER_CLIENT" 2560 720 move $((start_x + 320 + x / 2 + 20)) $((180 + y / 2 + 20)) press "$BTN_LEFT" \
    move $((start_x + 640)) 400 move $((other_x + 640)) 360 release "$BTN_LEFT" > /dev/null
  "$UMBRIEL" settle
  "$UMBRIEL" msg overview-close > /dev/null
  "$UMBRIEL" settle
  expect "overview drop" "\$p.output == \"$OTHER\""
}

for mover in workspace output unplug workspace_to_output scratchpad pin fullscreen column floating drag overview_drop; do
  "mover_$mover"
done

echo "the modal dialog followed its parent through every mover, and actions aimed at it moved the parent"
