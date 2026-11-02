#!/usr/bin/env bash
# harness: outputs=2
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly WINDOW_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-layer-window.log"
readonly PANEL_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-layer-panel.log"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/fullscreen-layer.png"

assert_drawn_pixel() {
  local y=$1 expected=$2 x=${3:-100} actual=
  grim -o HEADLESS-1 "$SHOT"
  actual=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel "$x" "$y")
  if [[ $actual != "$expected" ]]; then
    echo "expected RGB $expected at $x,$y, got $actual"
    return 1
  fi
}

assert_pixel() {
  "$UMBRIEL" settle > /dev/null
  assert_drawn_pixel "$@"
}

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false

[input.focus]
follows_mouse = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
home_x=$("$UMBRIEL" outputs | awk '$1 == "HEADLESS-1" {found = 1; next} found && /Position:/ {split($2, p, ","); print p[1]; exit}')
pointer_hold 2560 720 move "$((home_x + 640))" 360 mod none
"$UMBRIEL_SEAT_LOG_CLIENT" fullscreen-layer-window > "$WINDOW_LOG" 2>&1 &
await_events "$WINDOW_LOG" keyboard-enter 1
window_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "fullscreen-layer-window") | .id')
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
assert_pixel 20 "51 136 204"

# Hidden surfaces may not receive frame callbacks, so wait for compositor mapping instead of client readiness.
wait_layers() {
  local expected=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" layers --json | jq '[.[] | select(.mapped)] | length') == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected mapped layers: $("$UMBRIEL" layers --json)"
  return 1
}

assert_no_enter() {
  if [[ $(events "$PANEL_LOG" keyboard-enter) != 0 ]]; then
    echo "hidden top-layer panel received keyboard focus: $(cat "$PANEL_LOG")"
    return 1
  fi
}

key_count=0
for mode in on-demand exclusive; do
  "$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 "keyboard=$mode" > "$PANEL_LOG" 2>&1 &
  panel_pid=$!
  wait_layers 1
  assert_pixel 20 "51 136 204"
  "$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
  key_count=$((key_count + 1))
  await_events "$WINDOW_LOG" 'keyboard-key code=57 state=pressed' "$key_count"
  assert_no_enter

  "$UMBRIEL" msg window-focus "$window_id" > /dev/null
  "$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
  key_count=$((key_count + 1))
  await_events "$WINDOW_LOG" 'keyboard-key code=57 state=pressed' "$key_count"
  assert_no_enter

  overlay_log="$UMBRIEL_RUNTIME_DIR/fullscreen-overlay-$mode.log"
  "$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 overlay-layer "keyboard=$mode" release-on-escape > "$overlay_log" 2>&1 &
  overlay_pid=$!
  await_events "$overlay_log" keyboard-enter 1
  assert_pixel 20 "255 0 0"
  "$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57 tap 1
  await_events "$overlay_log" 'keyboard-key code=57 state=1' 1
  await_events "$overlay_log" keyboard-leave 1
  "$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
  key_count=$((key_count + 1))
  await_events "$WINDOW_LOG" 'keyboard-key code=57 state=pressed' "$key_count"
  assert_no_enter
  kill "$overlay_pid"
  wait "$overlay_pid" || true
  wait_layers 1
  assert_pixel 20 "51 136 204"
  "$UMBRIEL" msg window-toggle-fullscreen > /dev/null
  "$UMBRIEL" settle > /dev/null
  # Leaving fullscreen uncovers the panel: an exclusive one takes the seat back, an on-demand one waits for a click.
  if [[ $mode == on-demand ]]; then
    assert_no_enter
    "$UMBRIEL_POINTER_CLIENT" 2560 720 move "$((home_x + 100))" 20 click 272
  fi
  await_events "$PANEL_LOG" keyboard-enter 1
  "$UMBRIEL" msg window-toggle-fullscreen > /dev/null
  await_events "$PANEL_LOG" keyboard-leave 1
  assert_pixel 20 "51 136 204"
  "$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
  key_count=$((key_count + 1))
  await_events "$WINDOW_LOG" 'keyboard-key code=57 state=pressed' "$key_count"
  kill "$panel_pid"
  wait "$panel_pid" || true
  wait_layers 0

  other_log="$UMBRIEL_RUNTIME_DIR/other-output-$mode.log"
  "$UMBRIEL_LAYER_CLIENT" HEADLESS-2 40 "keyboard=$mode" > "$other_log" 2>&1 &
  other_pid=$!
  await_events "$other_log" keyboard-enter 1
  "$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
  await_events "$other_log" 'keyboard-key code=57 state=1' 1
  kill "$other_pid"
  wait "$other_pid" || true
  wait_layers 0
  "$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
  key_count=$((key_count + 1))
  await_events "$WINDOW_LOG" 'keyboard-key code=57 state=pressed' "$key_count"
done

# A fullscreen column can stay mapped while scrolling carries it completely out of the output. Zero insets make
# the strip's raw pan endpoints align fullscreen with the output instead of leaving a gap along its edge.
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[layout]
mode = "scrolling"
gap = 0
[layout.scrolling]
default_extent_fraction = 1.0
center_focused = "never"

[input.mouse]
scroll_wheel_step = 128

[appearance]
border_width = 0
outer_border_width = 0

[[window_rule]]
match.title = "^fullscreen-layer-foreground$"
default_output = "HEADLESS-1"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
foreground_log="$UMBRIEL_RUNTIME_DIR/fullscreen-layer-foreground.log"
"$UMBRIEL_SEAT_LOG_CLIENT" fullscreen-layer-foreground > "$foreground_log" 2>&1 &
await_events "$foreground_log" keyboard-enter 1
foreground_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "fullscreen-layer-foreground") | .id')
foreground_keys=0

assert_fullscreen_offscreen() {
  if ! "$UMBRIEL" windows --json | jq -e --arg id "$window_id" --argjson x "$home_x" \
    'any(.[]; .id == $id and .fullscreen and (.x + .w <= $x or .x >= $x + 1280))' > /dev/null; then
    echo "fullscreen column did not leave its output: $("$UMBRIEL" windows --json)"
    return 1
  fi
}

for mode in on-demand exclusive; do
  "$UMBRIEL" msg window-focus "$foreground_id" > /dev/null
  "$UMBRIEL" settle > /dev/null
  assert_fullscreen_offscreen
  visible_log="$UMBRIEL_RUNTIME_DIR/visible-top-$mode.log"
  "$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 "keyboard=$mode" > "$visible_log" 2>&1 &
  panel_pid=$!
  await_events "$visible_log" keyboard-enter 1
  assert_pixel 20 "32 32 32"
  "$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
  await_events "$visible_log" 'keyboard-key code=57 state=1' 1

  # A partially visible fullscreen column stays below top layers and cannot interrupt their keyboard input.
  "$UMBRIEL" msg layout-scroll-left > /dev/null
  "$UMBRIEL" settle > /dev/null
  assert_pixel 20 "32 32 32" 5
  if ! "$UMBRIEL" windows --json | jq -e --arg id "$window_id" --argjson x "$home_x" \
    'any(.[]; .id == $id and .fullscreen and .x < $x and .x + .w > $x)' > /dev/null; then
    echo "fullscreen column did not partially enter its output: $("$UMBRIEL" windows --json)"
    exit 1
  fi
  "$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
  await_events "$visible_log" 'keyboard-key code=57 state=1' 2
  if [[ $(events "$visible_log" keyboard-leave) != 0 ]]; then
    echo "partially visible fullscreen interrupted top-layer focus: $(cat "$visible_log")"
    exit 1
  fi

  for _ in $(seq 20); do "$UMBRIEL" msg layout-scroll-left > /dev/null; done
  assert_pixel 20 "51 136 204"
  await_events "$visible_log" keyboard-leave 1
  "$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
  foreground_keys=$((foreground_keys + 1))
  await_events "$foreground_log" 'keyboard-key code=57 state=pressed' "$foreground_keys"
  if [[ $(events "$visible_log" 'keyboard-key code=57 state=1') != 2 ]]; then
    echo "covered top layer retained keyboard input: $(cat "$visible_log")"
    exit 1
  fi
  for _ in $(seq 20); do "$UMBRIEL" msg layout-scroll-right > /dev/null; done
  "$UMBRIEL" settle > /dev/null
  assert_fullscreen_offscreen
  assert_pixel 20 "32 32 32"
  if [[ $mode == on-demand ]]; then
    if [[ $(events "$visible_log" keyboard-enter) != 1 ]]; then
      echo "uncovered on-demand layer took focus without a click: $(cat "$visible_log")"
      exit 1
    fi
    "$UMBRIEL_POINTER_CLIENT" 2560 720 move "$((home_x + 100))" 20 click 272
  fi
  await_events "$visible_log" keyboard-enter 2
  "$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
  await_events "$visible_log" 'keyboard-key code=57 state=1' 3
  kill "$panel_pid"
  wait "$panel_pid" || true
  wait_layers 0
  "$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
  foreground_keys=$((foreground_keys + 1))
  await_events "$foreground_log" 'keyboard-key code=57 state=pressed' "$foreground_keys"
done

# A pending scroll target cannot change stacking or keyboard eligibility before the window reaches it on screen.
sed -i '/^\[animation\]$/,/^\[/s/^enabled = false$/enabled = true/' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation.windows_move]
enabled = true
duration_ms = 1000
curve = "linear"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
animated_log="$UMBRIEL_RUNTIME_DIR/animated-top-exclusive.log"
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 keyboard=exclusive > "$animated_log" 2>&1 &
await_events "$animated_log" keyboard-enter 1
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" clock-freeze > /dev/null

for _ in $(seq 20); do "$UMBRIEL" msg layout-scroll-left > /dev/null; done
assert_drawn_pixel 20 "32 32 32"
"$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
await_events "$animated_log" 'keyboard-key code=57 state=1' 1
"$UMBRIEL" clock-advance 500 > /dev/null
assert_drawn_pixel 20 "32 32 32"
"$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
await_events "$animated_log" 'keyboard-key code=57 state=1' 2
if [[ $(events "$animated_log" keyboard-leave) != 0 ]]; then
  echo "animated fullscreen interrupted top-layer focus before covering the output: $(cat "$animated_log")"
  exit 1
fi

"$UMBRIEL" clock-advance 1000 > /dev/null
assert_drawn_pixel 20 "51 136 204"
await_events "$animated_log" keyboard-leave 1
for _ in $(seq 20); do "$UMBRIEL" msg layout-scroll-right > /dev/null; done
assert_drawn_pixel 20 "51 136 204"
if [[ $(events "$animated_log" keyboard-enter) != 1 ]]; then
  echo "top layer regained focus before fullscreen moved away: $(cat "$animated_log")"
  exit 1
fi
"$UMBRIEL" clock-advance 500 > /dev/null
assert_drawn_pixel 20 "32 32 32"
await_events "$animated_log" keyboard-enter 2
"$UMBRIEL_POINTER_CLIENT" 2560 720 tap 57
await_events "$animated_log" 'keyboard-key code=57 state=1' 3

echo "fullscreen coverage gates top-layer focus, including scrolling away and back; overlays remain interactive"
