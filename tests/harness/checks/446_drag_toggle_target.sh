#!/usr/bin/env bash
# input.window_drag_toggle retargets a live drag: the window's state changes
# only when the drag drops. So a drop that floats lands on the remembered
# floating size with the pointer still holding the same part of the window,
# toggling out and back returns the window to the strip with the column width
# the drag detached, and a window that refuses the target state (a fullscreen
# window cannot be pinned) still lands in the layout instead of staying
# detached with no way back.
set -euo pipefail

readonly BTN_LEFT=272
readonly BTN_RIGHT=273
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_count() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s)"
  return 1
}

window() {
  "$UMBRIEL" windows --json | jq -r --arg t "$1" ".[] | select(.title == \$t) | $2"
}

# Drag the named window from its center to (x, y), pressing the free button
# `presses` times on the way, and drop it there.
drag_toggling() {
  local title=$1 x=$2 y=$3 presses=$4
  local wx wy ww wh
  read -r wx wy ww wh < <(window "$title" '"\(.x) \(.y) \(.w) \(.h)"')
  local args=(move $((wx + ww / 2)) $((wy + wh / 2)) mod logo press "$BTN_LEFT" move "$x" "$y")
  for _ in $(seq "$presses"); do
    args+=(press "$BTN_RIGHT" release "$BTN_RIGHT")
  done
  args+=(move "$x" "$y" release "$BTN_LEFT" mod none)
  pointer "${args[@]}"
  sleep 0.9
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[input]
window_drag_toggle = "floating"

[layout.scrolling]
default_width_fraction = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client toggle-keep
wait_for_count 1
spawn_client toggle-wide
wait_for_count 2
sleep 0.5

# Give the window a floating size that differs from its column, so the drop can
# be told apart from a drag that never resized anything.
"$UMBRIEL" msg window-toggle-floating > /dev/null
sleep 0.6
"$UMBRIEL" msg window-set-width:0.3 > /dev/null
"$UMBRIEL" msg window-set-height:0.4 > /dev/null
sleep 0.8
read -r float_w float_h < <(window toggle-wide '"\(.w) \(.h)"')
"$UMBRIEL" msg window-toggle-floating > /dev/null
sleep 0.6
"$UMBRIEL" msg window-set-width:0.75 > /dev/null
sleep 0.8
read -r tiled_w tiled_h < <(window toggle-wide '"\(.w) \(.h)"')
neighbor_w=$(window toggle-keep .w)
if [[ $tiled_w == "$float_w" || $tiled_w == "$neighbor_w" ]]; then
  echo "setup needs a column width distinct from the float size and the default: ${tiled_w}/${float_w}/${neighbor_w}"
  exit 1
fi

# One press: the drop floats the window at its remembered size, and the pointer
# keeps its grip on the window across that resize.
drag_toggling toggle-wide 700 400 1
read -r floating x y w h < <(window toggle-wide '"\(.floating) \(.x) \(.y) \(.w) \(.h)"')
if [[ $floating != true ]]; then
  echo "one toggle did not float the dragged window: $("$UMBRIEL" windows --json)"
  exit 1
fi
if [[ $w != "$float_w" || $h != "$float_h" ]]; then
  echo "the float did not land on its remembered ${float_w}x${float_h} size: ${w}x${h}"
  exit 1
fi
if ((700 < x || 700 > x + w || 400 < y || 400 > y + h)); then
  echo "the pointer lost its grip on the window: pointer 700,400 outside ${w}x${h} at $x,$y"
  exit 1
fi

# Two presses: the drag ends up targeting the strip again, so the window drops
# back into it. Dropping past the last column opens a new one, which is where
# the width the drag detached has to reappear.
"$UMBRIEL" msg window-toggle-floating > /dev/null
sleep 0.6
"$UMBRIEL" msg window-set-width:0.75 > /dev/null
sleep 0.8
drag_toggling toggle-wide 1270 400 2
read -r floating w h < <(window toggle-wide '"\(.floating) \(.w) \(.h)"')
if [[ $floating != false ]]; then
  echo "toggling out and back left the window floating: $("$UMBRIEL" windows --json)"
  exit 1
fi
if [[ $w != "$tiled_w" || $h != "$tiled_h" ]]; then
  echo "the drop lost the detached column size: expected ${tiled_w}x${tiled_h}, got ${w}x${h}"
  exit 1
fi

# Pinned mode: one press pins the tiled window, and dragging the pinned window
# with one press puts it back where it was pinned from.
sed -i 's/window_drag_toggle = "floating"/window_drag_toggle = "pinned"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
sleep 0.3
drag_toggling toggle-wide 700 400 1
if [[ $(window toggle-wide .floating) != true ]]; then
  echo "pinned mode did not take the window out of the layout: $("$UMBRIEL" windows --json)"
  exit 1
fi
drag_toggling toggle-wide 400 300 1
if [[ $(window toggle-wide .floating) != false ]]; then
  echo "unpinning during a drag did not re-tile the window: $("$UMBRIEL" windows --json)"
  exit 1
fi

# A fullscreen window refuses to be pinned, so the drop has to fall back to the
# layout instead of leaving it detached.
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
sleep 0.8
fullscreen_title=$("$UMBRIEL" windows --json | jq -r '.[] | select(.focused) | .title')
if [[ -z $fullscreen_title ]]; then
  echo "no focused window to fullscreen: $("$UMBRIEL" windows --json)"
  exit 1
fi
drag_toggling "$fullscreen_title" 1270 360 1
if [[ $(window "$fullscreen_title" .floating) != false ]]; then
  echo "pinning a fullscreen window during a drag changed its state: $("$UMBRIEL" windows --json)"
  exit 1
fi
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
sleep 0.9
# In the layout its column is narrower than the output, and layout-directed
# focus can leave it. A detached window keeps the full output size and holds
# focus with no way back.
if [[ $(window "$fullscreen_title" .w) -ge $OUTPUT_W ]]; then
  echo "a refused pin left its window at output size: $("$UMBRIEL" windows --json)"
  exit 1
fi
"$UMBRIEL" msg window-focus-left > /dev/null
sleep 0.4
if [[ $("$UMBRIEL" windows --json | jq -r '.[] | select(.focused) | .title') == "$fullscreen_title" ]]; then
  echo "focus could not leave the dropped window: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "drag retargeting floats at the remembered size, restores the detached column size, and survives a refused pin"
