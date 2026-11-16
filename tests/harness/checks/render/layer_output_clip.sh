#!/usr/bin/env bash
# harness: outputs=2
# A layer surface pushed past its output's edge, and a subsurface hanging off it, are clipped to that output: neither
# renders on the neighbouring output nor takes pointer input there.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly LAYER_LOG="$UMBRIEL_RUNTIME_DIR/layer.log"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/layer-output-clip.png"
readonly RED='r > 0.9 && g < 0.1 && b < 0.1'
readonly BLUE='b > 0.9 && r < 0.1 && g < 0.1'

# HEADLESS-1 sits to the right of HEADLESS-2, so a layer surface on HEADLESS-1 with a negative left margin overflows
# into HEADLESS-2.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output.HEADLESS-1]
mode = "1280x720"
position = [1280, 0]

[output.HEADLESS-2]
mode = "1280x720"
position = [0, 0]

[colors]
backdrop = "#000000FF"

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$UMBRIEL_RUNTIME_DIR/layer.fifo"
env LAYER_CONTROL=1 "$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 overlay-layer popup-on-release \
  < "$UMBRIEL_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$UMBRIEL_RUNTIME_DIR/layer.fifo"
await_events "$LAYER_LOG" ready 1 "the overlay surface"

# The red 200x200 surface spans output-local x -100..100. A blue 200x200 subsurface below it spans x -150..50.
printf 'margins push 0 0 0 -100\n' >&"$layer_fd"
await_events "$LAYER_LOG" 'done push' 1 "the pushed surface"
printf 'child-color child FF0000FF\n' >&"$layer_fd"
await_events "$LAYER_LOG" 'done child' 1 "the subsurface"
printf 'child-move moved -50 200\n' >&"$layer_fd"
await_events "$LAYER_LOG" 'done moved' 1 "the moved subsurface"
"$UMBRIEL" settle

grim "$IMAGE"
read -r x y w h < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox "$RED")
if ((x != 1280 || y != 0 || w != 100 || h != 200)); then
  echo "the layer surface was not clipped to its output: red covers $x,$y ${w}x$h, expected 1280,0 100x200"
  exit 1
fi
read -r x y w h < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox "$BLUE")
if ((x != 1280 || y != 200 || w != 50 || h != 200)); then
  echo "the layer subsurface was not clipped to its output: blue covers $x,$y ${w}x$h, expected 1280,200 50x200"
  exit 1
fi

# A click on the overflow (surface-local x 50) must reach nothing; the one on the visible part lands at x 150. Both
# would reach the layer client in order over its one connection, so the second release frame means the first was seen.
"$UMBRIEL_POINTER_CLIENT" 2560 720 move 1230 100 click 272 > /dev/null
"$UMBRIEL_POINTER_CLIENT" 2560 720 move 1330 100 click 272 > /dev/null
await_events "$LAYER_LOG" pointer-release-frame 1 "the click on the visible part"
releases=$(grep '^pointer-release-frame' "$LAYER_LOG")
if [[ $releases != 'pointer-release-frame valid=true x=150 y=100' ]]; then
  echo "the layer surface took input outside its output, got:"
  echo "$releases"
  exit 1
fi

echo "layer surfaces and their subsurfaces are clipped to their output for rendering and input"
