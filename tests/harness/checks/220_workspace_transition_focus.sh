#!/usr/bin/env bash
# An outgoing workspace remains visible during its slide, but it must be visual only. With focus-follows-mouse enabled,
# a tiny pointer motion over that outgoing view used to focus it and reactivate its workspace. This reproduces the
# reported Mod+Wheel workspace switching failure with the real virtual pointer and modifier paths.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

# The wrapper survives only to pin the output dimensions every pointer call needs.
pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 &
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

active_title() {
  "$UMBRIEL" windows --json | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end'
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[input.focus]
follows_mouse = true

[keybinds]
"Mod+WheelDown" = "workspace-next"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client "transition-first"
wait_for_count 1
"$UMBRIEL" msg workspace-switch:2 > /dev/null
sleep 0.1
spawn_client "transition-second"
wait_for_count 2
"$UMBRIEL" msg workspace-switch:1 > /dev/null
sleep 0.1

# Slow only the transition under test. The setup switches stay fast so this check does not spend twenty seconds waiting
# for animations that are unrelated to the assertion.
sed -i 's/duration_ms = 1/duration_ms = 10000/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

windows=$("$UMBRIEL" windows --json)
first_x=$(jq -r '.[] | select(.title == "transition-first") | (.x + .w / 2 | round)' <<< "$windows")
first_y=$(jq -r '.[] | select(.title == "transition-first") | (.y + .h / 2 | round)' <<< "$windows")
pointer move "$first_x" "$first_y"

# Select workspace 2 through the reported binding. Keep this assertion separate from the motion so a broken wheel bind
# cannot masquerade as the transition hit-test failure.
pointer mod logo notch 1 mod none
if [[ $(active_title) != transition-second ]]; then
  echo "Mod+WheelDown did not focus workspace 2: $("$UMBRIEL" windows --json)"
  exit 1
fi

# The old view still occupies this point near the beginning of the slide. Clicking it must not make that visual snapshot
# interactive and switch back to workspace 1.
pointer click "$BTN_LEFT"
if [[ $(active_title) != transition-second ]]; then
  echo "outgoing transition view stole focus after a click: $("$UMBRIEL" windows --json)"
  exit 1
fi

# ---------------------------------------------------------------------------
# The same transition on a horizontally arranged output travels along X, so
# every presentation path has to carry both slide offset components.
# ---------------------------------------------------------------------------

readonly SHOT="$UMBRIEL_RUNTIME_DIR/transition-axis.png"

spawn_sized() {
  "$CLIENT" "$1" "$2" "$3" > /dev/null 2>&1 &
}

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r '[.[] | select(.active) | .name] | if length == 1 then .[0] else "none" end'
}

# Tiled windows report their tile origin, floating ones their own position, and both report the client's own buffer
# size. The harness client keeps the size it was spawned with, so every window below is spawned smaller than the box
# it is given and its reported box is exactly what it paints.
box_of() {
  "$UMBRIEL" windows --json | jq -r --arg t "$1" '.[] | select(.title == $t) | "\(.x) \(.y) \(.w) \(.h)"'
}

id_of() {
  "$UMBRIEL" windows --json | jq -r --arg t "$1" '.[] | select(.title == $t) | .id'
}

# Mean blue of a 20x20 patch. The harness client paints 0x5577AA, the backdrop is black, so a patch is either client
# content (blue well above 100) or uncovered output (blue at 0).
sample_blue() {
  magick "$SHOT" -crop "20x20+$1+$2" -format '%[fx:round(255*mean.b)]' info:
}

# Brightest blue inside a crop geometry: any client pixel in the region answers well above 100, an empty region
# answers 0. A region tolerates the eased slide passing through it instead of pinning one displacement.
region_blue() {
  magick "$SHOT" -crop "$1" -format '%[fx:round(255*maxima.b)]' info:
}

fast_animations() {
  sed -i 's/^duration_ms = 10000$/duration_ms = 1/' "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

slow_animations() {
  sed -i 's/^duration_ms = 1$/duration_ms = 10000/' "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

# A workspace switch finishes the live slide before it starts its own, so a pass through the neighbouring workspace
# with fast animations leaves every workspace at rest.
settle_to() {
  local target=$1 via=3
  [[ $target == 3 ]] && via=4
  fast_animations
  "$UMBRIEL" msg "workspace-switch:$via" > /dev/null
  "$UMBRIEL" msg "workspace-switch:$target" > /dev/null
  sleep 0.4
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[layout]
gap = 8

[output."HEADLESS-1"]
workspaces = 4
workspace_axis = "horizontal"

[[window_rule]]
match.title = "^axis-float$"
default_floating = true
default_floating_size_px = { width = 200, height = 200 }
default_position = { x = 900, y = 420, anchor = "top_left" }

[[window_rule]]
match.title = "^axis-pinned$"
default_floating = true
default_floating_size_px = { width = 200, height = 200 }
default_position = { x = 900, y = 60, anchor = "top_left" }
EOF
# This reload lands while the vertical slide above is still running: an axis change settles it and keeps the workspace
# the switch already committed to.
fast_animations
sleep 0.4
if [[ $(active_workspace) != 2 ]]; then
  echo "the axis reload dropped the committed workspace: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

# Horizontal workspaces scroll vertically, so a tiled window sits at the strip's left gap, centred on the scrolling
# axis. Its rendered pixels are exactly the box the IPC reports.
"$UMBRIEL" msg workspace-switch:3 > /dev/null
spawn_sized axis-tiled 600 300
wait_for_count 3
sleep 0.5
read -r tiled_x tiled_y tiled_w tiled_h <<< "$(box_of axis-tiled)"
if ((tiled_y < 60 || tiled_w < 200 || tiled_h < 100)); then
  echo "the vertical strip did not leave room around its tile: $(box_of axis-tiled)"
  exit 1
fi
tile_row=$((tiled_y + 40))
tile_near=$((tiled_x + 40))
tile_far=$((tiled_x + tiled_w - 24))
tile_above=$((tiled_y - 40))
grim "$SHOT"
rest_near=$(sample_blue "$tile_near" "$tile_row")
rest_far=$(sample_blue "$tile_far" "$tile_row")
rest_above=$(sample_blue "$tile_near" "$tile_above")
if ((rest_near < 100 || rest_far < 100 || rest_above > 10)); then
  echo "the tile does not rest where the IPC reports it: near=$rest_near far=$rest_far above=$rest_above" \
    "box=$(box_of axis-tiled)"
  exit 1
fi

slow_animations
"$UMBRIEL" msg workspace-switch:4 > /dev/null
sleep 0.5
grim "$SHOT"
slide_near=$(sample_blue "$tile_near" "$tile_row")
slide_far=$(sample_blue "$tile_far" "$tile_row")
slide_above=$(sample_blue "$tile_near" "$tile_above")
if ((slide_far > 10)); then
  echo "the tiled workspace did not travel along X on a horizontal output: blue=$slide_far at x=$tile_far"
  exit 1
fi
if ((slide_near < 100)); then
  echo "the outgoing tile left the output instead of sliding: blue=$slide_near at x=$tile_near"
  exit 1
fi
if ((slide_above > 10)); then
  echo "the horizontal slide displaced its workspace along Y: blue=$slide_above at y=$tile_above"
  exit 1
fi

# The outgoing tile is still under the pointer here, and it must stay a picture: clicking it may not reactivate
# workspace 3 the way the vertical axis case above must not reactivate workspace 1.
pointer move "$tile_near" "$tile_row"
pointer click "$BTN_LEFT"
if [[ $(active_workspace) != 4 ]]; then
  echo "the outgoing horizontal transition view stole its workspace back: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

# Floating content travels with its workspace, a pinned window never does.
settle_to 4
spawn_sized axis-float 200 200
wait_for_count 4
spawn_sized axis-pinned 200 200
wait_for_count 5
sleep 0.5
"$UMBRIEL" msg "window-focus-warp:$(id_of axis-pinned)" > /dev/null
"$UMBRIEL" msg window-toggle-pinned > /dev/null
sleep 0.3
read -r float_x float_y float_w float_h <<< "$(box_of axis-float)"
read -r pin_x pin_y pin_w _ <<< "$(box_of axis-pinned)"
float_inside_y=$((float_y + 60))
float_right=$((float_x + float_w + 40))
pin_inside=$((pin_y + 40))
pin_right=$((pin_x + pin_w + 40))
# The bands around the resting float. Nothing else rests in them, so a slide offset in either direction shows up as
# client content inside one of them, whatever displacement the eased animation has reached.
band_right="$((OUTPUT_W - float_x - float_w - 10))x${float_h}+$((float_x + float_w + 10))+${float_y}"
band_left="$((float_x - 10))x${float_h}+0+${float_y}"
band_below="${float_w}x$((OUTPUT_H - float_y - float_h - 10))+${float_x}+$((float_y + float_h + 10))"
band_above="${float_w}x80+${float_x}+$((float_y - 90))"
grim "$SHOT"
rest_float_right=$(sample_blue "$float_right" "$float_inside_y")
rest_pin_right=$(sample_blue "$pin_right" "$pin_inside")
if ((rest_float_right > 10 || rest_pin_right > 10)); then
  echo "the floating windows do not rest where their rules put them:" \
    "float=$(box_of axis-float) pinned=$(box_of axis-pinned)"
  exit 1
fi

# Backwards along the axis: workspace 3 is left of workspace 4, so its content leaves to the right.
slow_animations
"$UMBRIEL" msg workspace-switch:3 > /dev/null
sleep 0.5
grim "$SHOT"
slide_float_right=$(sample_blue "$float_right" "$float_inside_y")
slide_pin_inside=$(sample_blue "$pin_x" "$pin_inside")
slide_pin_right=$(sample_blue "$pin_right" "$pin_inside")
if ((slide_float_right < 100)); then
  echo "the floating window did not travel along X with its workspace: blue=$slide_float_right at x=$float_right"
  exit 1
fi
if ((slide_pin_inside < 100)); then
  echo "the pinned window disappeared during the horizontal slide: blue=$slide_pin_inside at x=$pin_x"
  exit 1
fi
if ((slide_pin_right > 10)); then
  echo "the pinned window travelled with the horizontal slide: blue=$slide_pin_right at x=$pin_right"
  exit 1
fi

# A fullscreen window covers its output exactly, so its own right edge uncovers as it travels.
settle_to 3
spawn_sized axis-fullscreen "$OUTPUT_W" "$OUTPUT_H"
wait_for_count 6
sleep 0.5
"$UMBRIEL" msg "window-focus-warp:$(id_of axis-fullscreen)" > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
sleep 0.5
grim "$SHOT"
rest_full=$(sample_blue 1240 400)
if ((rest_full < 100)); then
  echo "the fullscreen window does not cover its output at rest: blue=$rest_full at x=1240"
  exit 1
fi
slow_animations
"$UMBRIEL" msg workspace-switch:4 > /dev/null
sleep 0.5
grim "$SHOT"
slide_full_left=$(sample_blue 20 400)
slide_full_right=$(sample_blue 1240 400)
if ((slide_full_right > 10)); then
  echo "the fullscreen window did not travel along X: blue=$slide_full_right at x=1240"
  exit 1
fi
if ((slide_full_left < 100)); then
  echo "the outgoing fullscreen window left the output instead of sliding: blue=$slide_full_left at x=20"
  exit 1
fi
settle_to 3
"$UMBRIEL" msg "window-focus-warp:$(id_of axis-fullscreen)" > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
sleep 0.4

# Reloading the axis mid-transition: the committed workspace stays, both offsets are settled to zero, and the next
# switch travels on the new axis.
slow_animations
"$UMBRIEL" msg workspace-switch:4 > /dev/null
sleep 0.5
sed -i 's/^workspace_axis = "horizontal"$/workspace_axis = "vertical"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
sleep 0.5
if [[ $(active_workspace) != 4 ]]; then
  echo "the mid-transition axis reload dropped the committed workspace: $("$UMBRIEL" workspaces --json)"
  exit 1
fi
grim "$SHOT"
settled_near=$(sample_blue $((float_x + 4)) $((float_y + 4)))
settled_far=$(sample_blue $((float_x + float_w - 24)) $((float_y + float_h - 24)))
settled_right=$(region_blue "$band_right")
settled_left=$(region_blue "$band_left")
settled_below=$(region_blue "$band_below")
settled_above=$(region_blue "$band_above")
if ((settled_near < 100 || settled_far < 100)); then
  echo "the axis reload did not settle the slide at rest: near-origin=$settled_near far-corner=$settled_far"
  exit 1
fi
if ((settled_right > 10 || settled_left > 10)); then
  echo "a horizontal slide offset survived the axis reload: right=$settled_right left=$settled_left"
  exit 1
fi
if ((settled_below > 10 || settled_above > 10)); then
  echo "a vertical slide offset survived the axis reload: below=$settled_below above=$settled_above"
  exit 1
fi

# Vertical workspaces again: workspace 3 is above workspace 4, so its content leaves downwards, not sideways.
"$UMBRIEL" msg workspace-switch:3 > /dev/null
sleep 0.8
grim "$SHOT"
next_below=$(region_blue "$band_below")
next_right=$(region_blue "$band_right")
if ((next_below < 100)); then
  echo "the switch after the axis reload did not travel along Y: blue=$next_below below the float"
  exit 1
fi
if ((next_right > 10)); then
  echo "the switch after the axis reload still travelled along X: blue=$next_right right of the float"
  exit 1
fi

# Settling a live slide follows the layout effect of the reload, not the axis value: a reload that only widens the
# layout gap has to leave the transition at rest as well.
settle_to 3
slow_animations
"$UMBRIEL" msg workspace-switch:4 > /dev/null
sleep 0.5
sed -i 's/^gap = 8$/gap = 24/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
sleep 0.5
if [[ $(active_workspace) != 4 ]]; then
  echo "the mid-transition gap reload dropped the committed workspace: $("$UMBRIEL" workspaces --json)"
  exit 1
fi
grim "$SHOT"
gap_inside=$(sample_blue $((float_x + 4)) $((float_y + 4)))
gap_below=$(region_blue "$band_below")
gap_right=$(region_blue "$band_right")
if ((gap_inside < 100 || gap_below > 10 || gap_right > 10)); then
  echo "a layout reload that keeps the axis did not settle the live slide:" \
    "inside=$gap_inside below=$gap_below right=$gap_right"
  exit 1
fi

echo "outgoing workspace transition views are non-interactive and slides follow the output's workspace axis"
