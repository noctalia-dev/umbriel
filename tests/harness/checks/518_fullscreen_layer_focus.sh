#!/usr/bin/env bash
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly WINDOW_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-layer-window.log"
readonly PANEL_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-layer-panel.log"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/fullscreen-layer.png"

assert_pixel() {
  local y=$1 expected=$2 actual=
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$SHOT"
  actual=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel 100 "$y")
  if [[ $actual != "$expected" ]]; then
    echo "expected RGB $expected at 100,$y, got $actual"
    return 1
  fi
}

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false

[input.focus]
follows_mouse = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
pointer_hold 1280 720 move 640 360 mod none
"$UMBRIEL_SEAT_LOG_CLIENT" fullscreen-layer-window > "$WINDOW_LOG" 2>&1 &
await_events "$WINDOW_LOG" keyboard-enter 1
window_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "fullscreen-layer-window") | .id')
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
assert_pixel 20 "51 136 204"

"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 log-configures > "$UMBRIEL_RUNTIME_DIR/fullscreen-layer-bar.log" 2>&1 &
await_events "$UMBRIEL_RUNTIME_DIR/fullscreen-layer-bar.log" configure 1
assert_pixel 20 "51 136 204"

"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 keyboard=on-demand > "$PANEL_LOG" 2>&1 &
panel_pid=$!
await_events "$PANEL_LOG" keyboard-enter 1
assert_pixel 60 "32 32 32"
assert_pixel 20 "51 136 204"
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 overlay-layer > "$UMBRIEL_RUNTIME_DIR/fullscreen-layer-overlay.log" 2>&1 &
overlay_pid=$!
await_events "$UMBRIEL_RUNTIME_DIR/fullscreen-layer-overlay.log" ready 1
assert_pixel 60 "255 0 0"
kill "$overlay_pid"
wait "$overlay_pid" || true
assert_pixel 60 "32 32 32"
"$UMBRIEL_POINTER_CLIENT" 1280 720 tap 57
await_events "$PANEL_LOG" 'keyboard-key code=57 state=1' 1

"$UMBRIEL" msg "window-focus:$window_id" > /dev/null
await_events "$WINDOW_LOG" keyboard-enter 2
await_events "$PANEL_LOG" keyboard-leave 1
assert_pixel 60 "51 136 204"
kill "$panel_pid"
wait "$panel_pid" || true

"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 keyboard=exclusive release-on-escape > "$PANEL_LOG" 2>&1 &
panel_pid=$!
await_events "$PANEL_LOG" keyboard-enter 1
assert_pixel 60 "32 32 32"
"$UMBRIEL_POINTER_CLIENT" 1280 720 tap 1
await_events "$PANEL_LOG" 'keyboard-key code=1 state=0' 1
await_events "$WINDOW_LOG" keyboard-enter 3
assert_pixel 60 "51 136 204"
"$UMBRIEL_POINTER_CLIENT" 1280 720 tap 57
await_events "$WINDOW_LOG" 'keyboard-key code=57 state=pressed' 1
kill "$panel_pid"
wait "$panel_pid" || true

"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 keyboard=exclusive > "$PANEL_LOG" 2>&1 &
panel_pid=$!
await_events "$PANEL_LOG" keyboard-enter 1
assert_pixel 60 "32 32 32"
kill "$panel_pid"
wait "$panel_pid" || true
await_events "$WINDOW_LOG" keyboard-enter 4
assert_pixel 60 "51 136 204"
"$UMBRIEL_POINTER_CLIENT" 1280 720 tap 1
await_events "$WINDOW_LOG" 'keyboard-key code=1 state=pressed' 1

echo "focused top-layer panels stay visible above fullscreen and return keyboard input when dismissed"
