#!/usr/bin/env bash
# With follows_mouse, a bar's grabbing menu must survive the pointer crossing a window on its way to the menu: hover
# never ends a popup grab. A click on that window still dismisses the menu and focuses the window.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly LEFT_BUTTON=272
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly WINDOW_LOG="$UMBRIEL_RUNTIME_DIR/layer-popup-hover-window.log"
readonly PANEL_LOG="$UMBRIEL_RUNTIME_DIR/layer-popup-hover-panel.log"
readonly KEYBOARD_LOG="$UMBRIEL_RUNTIME_DIR/layer-popup-hover-keyboard.log"

if [[ ! -x $POINTER || ! -x $OBSERVER || ! -x $LAYER_CLIENT ]]; then
  echo "layer popup hover helpers are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'
[input.focus]
follows_mouse = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

refuse_events() {
  local file=$1 event=$2 limit=$3 label=$4
  local seen
  seen=$(events "$file" "$event")
  if ((seen > limit)); then
    echo "$label received $seen '$event', expected at most $limit: $(tr '\n' '|' < "$file")"
    exit 1
  fi
}

# The headless seat has no keyboard until a virtual one appears, and without one no client ever receives a keyboard
# enter. This connection owns that keyboard for the rest of the check.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" mod none pause 60000 > "$KEYBOARD_LOG" 2>&1 &

"$OBSERVER" layer-popup-hover-window > "$WINDOW_LOG" 2>&1 &
await_events "$WINDOW_LOG" keyboard-enter 1 "the window"

"$LAYER_CLIENT" HEADLESS-1 40 popup-on-click > "$PANEL_LOG" 2>&1 &
await_events "$PANEL_LOG" ready 1 "the bar"

# Open the menu from the bar, then cross the window below the bar on the way to the menu at the bar's left edge.
pointer_hold "$OUTPUT_W" "$OUTPUT_H" move $((OUTPUT_W / 2)) 20 click "$LEFT_BUTTON" \
  -- move $((OUTPUT_W / 2)) 300 move 300 300 move 40 60 mark crossed hold move $((OUTPUT_W / 2)) 400 click "$LEFT_BUTTON"
await_events "$PANEL_LOG" popup-mapped 1 "the bar"
await_events "$WINDOW_LOG" keyboard-leave 1 "the window"

pointer_step crossed
"$UMBRIEL" settle > /dev/null
refuse_events "$PANEL_LOG" popup-done 0 "the bar"
refuse_events "$WINDOW_LOG" keyboard-enter 1 "the window"

pointer_release
await_events "$PANEL_LOG" popup-done 1 "the bar"
await_events "$WINDOW_LOG" keyboard-enter 2 "the window"

echo "hovering a window keeps a layer popup open, and clicking that window dismisses it"
