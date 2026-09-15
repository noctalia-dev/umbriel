#!/usr/bin/env bash
# A layer surface that asks for on-demand keyboard interactivity takes keyboard focus as it maps, which launchers,
# quick terminals, and panels rely on. Unlike an exclusive layer it holds no grab, so focusing a window afterward
# moves the keyboard back.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly WINDOW_LOG="$UMBRIEL_RUNTIME_DIR/layer-focus-window.log"
readonly PANEL_LOG="$UMBRIEL_RUNTIME_DIR/layer-focus-panel.log"
readonly KEYBOARD_LOG="$UMBRIEL_RUNTIME_DIR/layer-focus-keyboard.log"

if [[ ! -x $POINTER || ! -x $OBSERVER || ! -x $LAYER_CLIENT ]]; then
  echo "layer focus helpers are not available"
  exit 1
fi

events() {
  # The window observer suffixes its enter with the held-key count, so events are matched as line prefixes.
  grep -c "^$2" "$1" 2>/dev/null || true
}

await_events() {
  local file=$1 event=$2 expected=$3 label=$4
  for _ in $(seq 60); do
    (($(events "$file" "$event") >= expected)) && return 0
    sleep 0.1
  done
  echo "timed out waiting for $expected '$event' on $label: $(tr '\n' '|' < "$file")"
  return 1
}

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

"$OBSERVER" layer-focus-window > "$WINDOW_LOG" 2>&1 &
window_id=
for _ in $(seq 60); do
  window_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "layer-focus-window") | .id')
  [[ -n $window_id && $window_id != null ]] && break
  sleep 0.1
done
if [[ -z $window_id || $window_id == null ]]; then
  echo "the observer window was not registered: $("$UMBRIEL" windows --json | jq -c 'map(.title)')"
  exit 1
fi
# The window is focused because it just mapped, so the enter also proves the keyboard reached the seat.
await_events "$WINDOW_LOG" keyboard-enter 1 "the window"
refuse_events "$WINDOW_LOG" keyboard-leave 0 "the window"

"$LAYER_CLIENT" HEADLESS-1 40 keyboard=on-demand > "$PANEL_LOG" 2>&1 &
await_events "$PANEL_LOG" ready 1 "the panel"

# Mapping the panel is the whole transition under test: the panel gains the keyboard and the window loses it.
await_events "$PANEL_LOG" keyboard-enter 1 "the panel"
await_events "$WINDOW_LOG" keyboard-leave 1 "the window"

"$UMBRIEL" msg "window-focus:$window_id" > /dev/null
await_events "$WINDOW_LOG" keyboard-enter 2 "the window"
await_events "$PANEL_LOG" keyboard-leave 1 "the panel"
refuse_events "$PANEL_LOG" keyboard-enter 1 "the panel"

echo "an on-demand layer surface takes keyboard focus when it maps and releases it to a focused window"
