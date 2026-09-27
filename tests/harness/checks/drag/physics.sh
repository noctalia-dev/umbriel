#!/usr/bin/env bash
# Mod+drag deforms a held floating window as an elastic sheet that trails past its own box, relaxes back to a
# rectangular box after release, stays rigid on its overview card, and hands its frozen deformation to the close
# snapshot when the window closes mid-drag. With physics off the same drag stays rigid and leaves nothing running,
# held or released.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly DRAG_DX=180
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/drag-physics.png"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
drag_opacity = 1.0
[appearance.shadow]
enabled = false
[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"
[overview]
zoom = 0.5
[animation]
duration_ms = 1
curve = "linear"
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = true
duration_ms = 3000
[animation.windows_drag]
physics = true
[[window_rule]]
match.title = "^physics$"
default_floating = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

green_count() { "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.9 && r < 0.1 && b < 0.1' "$1"; }

spawn() {
  FILL_COLOR=0xFF00FF00 "$UMBRIEL_UNMAP_CLIENT" physics 400 300 > "$UMBRIEL_RUNTIME_DIR/physics.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "physics")')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]]
  "$UMBRIEL" settle > /dev/null
}

# Mod must be held before the button press and released after, or it leaks into the next drag. The grabbed corner
# tracks the pointer exactly, so the window's rigid (undeformed) position is the grab point plus this delta; the
# position IPC reports is the layout target and updates when the drag finishes.
drag_right() {
  local grab_x=$1 grab_y=$2
  pointer_hold "$OUTPUT_W" "$OUTPUT_H" move "$grab_x" "$grab_y" mod super press "$BTN_LEFT" \
    move $((grab_x + 60)) "$grab_y" move $((grab_x + 120)) "$grab_y" move $((grab_x + DRAG_DX)) "$grab_y" \
    -- release "$BTN_LEFT" mod none
}

# settle refuses while an animation runs on the frozen clock, so the clock advances between attempts until drag physics
# relaxes.
settle_frozen() {
  for _ in $(seq 40); do
    "$UMBRIEL" clock-advance 50 > /dev/null
    if "$UMBRIEL" settle > /dev/null 2>&1; then
      return 0
    fi
  done
  "$UMBRIEL" settle > /dev/null
}

spawn
read -r x y w h id < <(jq -r '"\(.x) \(.y) \(.w) \(.h) \(.id)"' <<< "$window")

# Grab the window near its top-left and pull it right quickly: the far (right) edge trails, so green appears left of
# the window's rigid new left edge, where the trailing sheet is drawn past the box.
"$UMBRIEL" clock-freeze
grab_x=$((x + 40))
grab_y=$((y + 40))
drag_right "$grab_x" "$grab_y"
"$UMBRIEL" clock-advance 16 > /dev/null
grim "$IMAGE"
new_x=$((x + DRAG_DX))
if (( $(green_count "8x40+$((new_x - 10))+$((y + h / 2))") < 100 )); then
  echo "no deformation trailed past the dragged window's box"
  exit 1
fi
pointer_release

# After release the sheet relaxes back into the window box.
settle_frozen
grim "$IMAGE"
window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "physics")')
read -r x y w h < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
if (( $(green_count "8x40+$((x - 10))+$((y + h / 2))") > 0 )); then
  echo "the sheet did not settle back into the window box after release"
  exit 1
fi
if (( $(green_count "${w}x${h}+${x}+${y}") < w * h * 9 / 10 )); then
  echo "the settled window is not drawn plainly"
  exit 1
fi

# The overview opened while the sheet settles: the card is drawn rigid. The 1280x720 output previews at half size from
# 320,180.
drag_right $((x + 40)) $((y + 40))
pointer_release
"$UMBRIEL" clock-advance 16 > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" clock-advance 50 > /dev/null
grim "$IMAGE"
card_x=$((320 + (x + DRAG_DX) / 2))
card_y=$((180 + y / 2))
edges=$(($(green_count "20x$((h / 2 - 20))+$((card_x - 22))+$((card_y + 10))")
  + $(green_count "20x$((h / 2 - 20))+$((card_x + w / 2 + 2))+$((card_y + 10))")))
if ((edges > 0)); then
  echo "the overview card kept the settling deformation: $edges green pixels past its edges"
  exit 1
fi
if (( $(green_count "$((w / 2))x$((h / 2))+${card_x}+${card_y}") < w * h / 4 * 9 / 10 )); then
  echo "the overview card does not fill its box with the dragged window"
  exit 1
fi
"$UMBRIEL" msg overview-close > /dev/null
settle_frozen
window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "physics")')
read -r x y w h < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")

# Close during a drag: the snapshot inherits the frozen deformation and fades out with it.
grab_x=$((x + 40))
grab_y=$((y + 40))
drag_right "$grab_x" "$grab_y"
"$UMBRIEL" clock-advance 16 > /dev/null
new_x=$((x + DRAG_DX))
"$UMBRIEL" msg "window-close:$id" > /dev/null
# Only the snapshot may be left to probe: the live window must have left the window list first.
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "physics")')
  [[ -z $window ]] && break
  sleep 0.025
done
[[ -z $window ]] || { echo "the dragged window never unmapped after the close request"; exit 1; }
"$UMBRIEL" clock-advance 100 > /dev/null
grim "$IMAGE"
if (( $(green_count "8x40+$((new_x - 10))+$((y + h / 2))") < 50 )); then
  echo "the close snapshot did not keep the frozen deformation"
  exit 1
fi
pointer_release
"$UMBRIEL" clock-advance 4000 > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
if (( $(green_count "${OUTPUT_W}x${OUTPUT_H}+0+0") > 0 )); then
  echo "the close snapshot did not retire"
  exit 1
fi
"$UMBRIEL" clock-resume

# physics = false: the drag stays rigid and runs no animation, held or released.
sed -i 's/^physics = true$/physics = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
spawn
read -r x y w h id < <(jq -r '"\(.x) \(.y) \(.w) \(.h) \(.id)"' <<< "$window")
"$UMBRIEL" clock-freeze
grab_x=$((x + 40))
grab_y=$((y + 40))
drag_right "$grab_x" "$grab_y"
if ! "$UMBRIEL" settle > /dev/null 2>&1; then
  echo "physics = false left an animation running while the drag is held"
  exit 1
fi
grim "$IMAGE"
new_x=$((x + DRAG_DX))
# A 10 px band 2 px clear of the rigid box, on all four sides.
band=0
for strip in "$((w + 24))x10+$((new_x - 12))+$((y - 12))" "$((w + 24))x10+$((new_x - 12))+$((y + h + 2))" \
  "10x$((h + 4))+$((new_x - 12))+$((y - 2))" "10x$((h + 4))+$((new_x + w + 2))+$((y - 2))"; do
  band=$((band + $(green_count "$strip")))
done
if ((band > 0)); then
  echo "physics = false still deformed the dragged window past its box: $band green pixels around it"
  exit 1
fi
pointer_release
if ! "$UMBRIEL" settle > /dev/null 2>&1; then
  echo "physics = false left an animation running after release"
  exit 1
fi
"$UMBRIEL" clock-resume

echo "drag physics deformed the held window, settled after release, handed its deformation to the close snapshot, and stayed rigid with physics off"
