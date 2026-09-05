#!/usr/bin/env bash
# harness: outputs=1
# Retargeting a drag to floating resizes the window to the size it will drop at,
# and that resize animates like any other. The window keeps its tiled state
# until the drop, so the client's ack of the new size arrives while it is still
# tiled and must not be mistaken for an interactive resize tracking the pointer.
set -euo pipefail

readonly TITLE=drag-toggle-animation
readonly BTN_LEFT=272
readonly BTN_RIGHT=273
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

wait_for_box() {
  local expected=$1 actual=
  for _ in $(seq 100); do
    actual=$("$UMBRIEL" windows --json | jq -r --arg t "$TITLE" '.[] | select(.title == $t) | "\(.w)x\(.h)"')
    [[ $actual == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected window $expected, got $actual"
  return 1
}

# Width of everything the output is drawing. The dragged window is the only
# window, so this is its presented width, clipped at the output edge.
capture_width() {
  local image="$UMBRIEL_RUNTIME_DIR/$1.png"
  grim "$image"
  magick "$image" -alpha off -colorspace gray -threshold 1% \
    -bordercolor black -border 1 -trim -format '%w\n' info:
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[input]
window_drag_toggle = "floating"

[animation]
duration_ms = 2000
curve = "linear"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[layout.scrolling]
default_width_fraction = 1.0
EOF
"$UMBRIEL" msg config-reload > /dev/null

foot --title="$TITLE" sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_box 1264x704

# Give it a floating size well below its column, so the retarget has a visible
# distance to animate, then tile it again for the drag to start from.
"$UMBRIEL" msg window-toggle-floating > /dev/null
sleep 0.4
"$UMBRIEL" msg window-set-width:0.25 > /dev/null
"$UMBRIEL" msg window-set-height:0.3 > /dev/null
wait_for_box 320x216
"$UMBRIEL" msg window-toggle-floating > /dev/null
wait_for_box 1264x704
# The re-tile animates too; sample only once it has settled.
sleep 2.5
tiled_width=$(capture_width tiled)
if ((tiled_width < 1264)); then
  echo "setup did not fill the output with the tiled window: $tiled_width"
  exit 1
fi

# Hold the drag open while the animation runs, and sample it twice on the way.
pointer move 640 360 mod logo press "$BTN_LEFT" move 640 400 \
  press "$BTN_RIGHT" release "$BTN_RIGHT" pause 4000 move 641 400 release "$BTN_LEFT" mod none &
sleep 0.45
early=$(capture_width early)
sleep 0.8
late=$(capture_width late)
sleep 1.6
settled=$(capture_width settled)
sleep 2.0

if ((early <= late || late <= settled)); then
  echo "the retarget did not animate: widths ${early} -> ${late} -> ${settled} (expected a shrinking presented size)"
  exit 1
fi
if ((settled != 320)); then
  echo "the animation did not settle on the floating width: $settled"
  exit 1
fi
if [[ $("$UMBRIEL" windows --json | jq -r --arg t "$TITLE" '.[] | select(.title == $t) | .floating') != true ]]; then
  echo "the drop did not float the window: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "the drag retarget animated the presented size down to the floating size: ${early} -> ${late} -> ${settled}"
