#!/usr/bin/env bash
# A layer popup crosses a later bar, follows its parent, respects higher layers, and redraws after overlapping resizes.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly LAYER="$UMBRIEL_LAYER_CLIENT"
readonly POINTER="$UMBRIEL_POINTER_CLIENT"
readonly PANEL_LOG="$UMBRIEL_RUNTIME_DIR/panel.log"
readonly SIBLING_LOG="$UMBRIEL_RUNTIME_DIR/sibling.log"
readonly OVERLAY_LOG="$UMBRIEL_RUNTIME_DIR/overlay.log"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/popup.png"
readonly YELLOW='r > 0.95 && g > 0.6 && g < 0.75 && b < 0.05'

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$UMBRIEL_RUNTIME_DIR/panel.fifo"
env LAYER_CONTROL=1 "$LAYER" HEADLESS-1 40 popup-on-release \
  < "$UMBRIEL_RUNTIME_DIR/panel.fifo" > "$PANEL_LOG" 2>&1 &
panel_pid=$!
exec {panel_fd}> "$UMBRIEL_RUNTIME_DIR/panel.fifo"
await_events "$PANEL_LOG" ready 1 "the parent bar"

"$POINTER" 1280 720 move 80 20 click 272 > /dev/null
await_events "$PANEL_LOG" popup-mapped 1 "the layer popup"
"$UMBRIEL" settle
grim "$IMAGE"
visible=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count "$YELLOW")
if ((visible != 6400)); then
  echo "the initial popup did not render fully: expected 6400 yellow pixels, got $visible"
  exit 1
fi

"$LAYER" HEADLESS-1 40 > "$SIBLING_LOG" 2>&1 &
await_events "$SIBLING_LOG" ready 1 "the later sibling bar"
"$UMBRIEL" settle
grim "$IMAGE"
visible=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count "$YELLOW")
if ((visible != 6400)); then
  echo "the sibling bar covered the popup: expected 6400 yellow pixels, got $visible"
  exit 1
fi

printf 'margins moved 30 0 0 0\n' >&"$panel_fd"
await_events "$PANEL_LOG" 'done moved' 1 "the moved parent bar"
"$UMBRIEL" settle
grim "$IMAGE"
read -r x y w h < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox "$YELLOW")
if ((x != 80 || y != 51 || w != 80 || h != 80)); then
  echo "the popup did not follow its parent: got $x,$y ${w}x$h"
  exit 1
fi

"$LAYER" HEADLESS-1 40 overlay-layer > "$OVERLAY_LOG" 2>&1 &
overlay_pid=$!
await_events "$OVERLAY_LOG" ready 1 "the higher overlay surface"
"$UMBRIEL" settle
grim "$IMAGE"
visible=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count "$YELLOW")
if ((visible != 0)); then
  echo "the top-layer popup escaped above the overlay: $visible yellow pixels"
  exit 1
fi
kill "$overlay_pid"
wait "$overlay_pid" 2>/dev/null || true
"$UMBRIEL" settle
grim "$IMAGE"
visible=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count "$YELLOW")
if ((visible != 6400)); then
  echo "the popup did not reappear after the overlay closed: $visible yellow pixels"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output."HEADLESS-1"]
scale = 1.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

resize_popup() {
  local label=$1 width=$2 height=$3
  local expected=$((width * 3 / 2 * (height * 3 / 2)))
  printf 'popup-resize %s %s %s\n' "$label" "$width" "$height" >&"$panel_fd"
  await_events "$PANEL_LOG" "done $label" 1 "redraw after overlapping popup repositions"
  "$UMBRIEL" settle
  grim "$IMAGE"
  visible=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count "$YELLOW")
  if ((visible != expected)); then
    echo "the resized popup did not redraw fully: expected $expected yellow pixels, got $visible"
    exit 1
  fi
}

resize_popup grown 188 392
resize_popup shrunk 120 180
replies=$(sed -n '/^popup-repositioned /p' "$PANEL_LOG")
expected_replies=$(cat <<'EOF'
popup-repositioned token=1 width=180 height=384
popup-repositioned token=0 width=188 height=392
popup-repositioned token=2 width=112 height=172
popup-repositioned token=0 width=120 height=180
EOF
)
if [[ $replies != "$expected_replies" ]]; then
  echo "overlapping popup requests lost a token, changed placement, or replied out of order: $replies"
  exit 1
fi

kill "$panel_pid"
wait "$panel_pid" 2>/dev/null || true
"$UMBRIEL" settle
grim "$IMAGE"
visible=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count "$YELLOW")
if ((visible != 0)); then
  echo "the destroyed parent's popup remained visible: $visible yellow pixels"
  exit 1
fi
echo "layer popups preserve stacking and redraw after every overlapping reposition request"
