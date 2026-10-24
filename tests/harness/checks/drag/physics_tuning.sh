#!/usr/bin/env bash
# The [animation.windows_drag] tuning keys reach the simulation: with
# pointer_response = 0 a drag loads no deformation at all, while the default
# response trails the sheet past the window's box. Ignoring the keys leaves a
# trail in the first case, which is the red this check pins.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly DRAG_DX=180
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/drag-tuning.png"
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
[animation]
duration_ms = 1
curve = "linear"
[animation.windows_in]
enabled = false
[animation.windows_drag]
physics = true
pointer_response = 0.0
[[window_rule]]
match.title = "^tuning$"
default_floating = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

green_count() { "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.9 && r < 0.1 && b < 0.1' "$1"; }

FILL_COLOR=0xFF00FF00 "$UMBRIEL_UNMAP_CLIENT" tuning 400 300 > /dev/null 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "tuning")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
"$UMBRIEL" settle > /dev/null

# Mod must be held before the button press and released after, or it leaks into
# the next drag. The grabbed corner tracks the pointer exactly, so the rigid
# window lands at the grab point plus this delta.
drag_right() {
  local grab_x=$1 grab_y=$2
  pointer_hold "$OUTPUT_W" "$OUTPUT_H" move "$grab_x" "$grab_y" mod super press "$BTN_LEFT" \
    move $((grab_x + 60)) "$grab_y" move $((grab_x + 120)) "$grab_y" move $((grab_x + DRAG_DX)) "$grab_y" \
    -- release "$BTN_LEFT" mod none
}

# settle refuses while an animation runs on the frozen clock, so the clock
# advances between attempts until drag physics relaxes.
settle_frozen() {
  for _ in $(seq 40); do
    "$UMBRIEL" clock-advance 50 > /dev/null
    if "$UMBRIEL" settle > /dev/null 2>&1; then
      return 0
    fi
  done
  "$UMBRIEL" settle > /dev/null
}

read -r x y w h < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
"$UMBRIEL" clock-freeze
drag_right $((x + 40)) $((y + 40))
"$UMBRIEL" clock-advance 16 > /dev/null
grim "$IMAGE"
if (( $(green_count "8x40+$((x + DRAG_DX - 10))+$((y + h / 2))") != 0 )); then
  echo "pointer_response = 0 still deformed the sheet past the window's box"
  exit 1
fi
pointer_release
settle_frozen

# The default response must trail: the first case is only meaningful if the
# same setup deforms when the knob is back at its default.
sed -i 's/^pointer_response = 0.0$/pointer_response = 2.0/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "tuning")')
read -r x y w h < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
drag_right $((x + 40)) $((y + 40))
"$UMBRIEL" clock-advance 16 > /dev/null
grim "$IMAGE"
if (( $(green_count "8x40+$((x + DRAG_DX - 10))+$((y + h / 2))") < 100 )); then
  echo "the default pointer_response did not trail the sheet past the window's box"
  exit 1
fi
pointer_release

echo "windows_drag tuning reaches the drag physics"
