#!/usr/bin/env bash
# Independent floats are revealed above fullscreen while the stacking layout has focus.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly SHOT="$UMBRIEL_RUNTIME_DIR/floating-above-fullscreen.png"
readonly APP_ID=floating-above-fullscreen

windows() { "$UMBRIEL" windows --json; }

wait_for_window() {
  local title=$1
  for _ in $(seq 80); do
    if windows | jq -e --arg title "$title" 'any(.[]; .title == $title)' >/dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "window $title did not map: $(windows)"
  return 1
}

assert_active() {
  local title=$1
  if ! windows | jq -e --arg title "$title" 'any(.[]; .title == $title and .active)' >/dev/null; then
    echo "expected $title to own keyboard focus: $(windows)"
    return 1
  fi
}

wait_for_absent() {
  local title=$1
  for _ in $(seq 80); do
    if windows | jq -e --arg title "$title" 'all(.[]; .title != $title)' >/dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "window $title did not unmap: $(windows)"
  return 1
}

assert_fullscreen() {
  if ! "$UMBRIEL" tearing --json | jq -e \
    'any(.surfaces[]; .title == "fullscreen-main" and .fullscreen)' >/dev/null; then
    echo "focusing a float changed the main window fullscreen state: $(windows)"
    return 1
  fi
}

assert_pixel() {
  local x=$1 y=$2 expected=$3 context=$4 actual
  actual=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel "$x" "$y")
  if [[ $actual != "$expected" ]]; then
    echo "$context: expected RGB $expected at $x,$y, got $actual"
    return 1
  fi
}

capture() {
  "$UMBRIEL" settle >/dev/null
  assert_fullscreen
  grim -o HEADLESS-1 "$SHOT"
}

cat >>"$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false

[input.focus]
follows_mouse = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
drag_opacity = 1.0
[appearance.shadow]
enabled = false

[[window_rule]]
match.app_id = "^floating-above-fullscreen$"
default_workspace = 1
default_floating = true
default_focused = true
default_position = { x = 600, y = 160, anchor = "top_left" }

[[window_rule]]
match.app_id = "^floating-above-fullscreen$"
match.title = "^fullscreen-main$"
default_floating = false
default_fullscreen = true

[[window_rule]]
match.app_id = "^floating-above-fullscreen$"
match.title = "^unfocused-float$"
default_focused = false
default_position = { x = 200, y = 160, anchor = "top_left" }

[[window_rule]]
match.app_id = "^floating-default-focus$"
default_workspace = 1
default_floating = true
default_position = { x = 950, y = 160, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload >/dev/null

env APP_ID="$APP_ID" FILL_COLOR=0xFF0000FF RESIZE_FILL_COLOR=0xFF0000FF \
  "$UMBRIEL_UNMAP_CLIENT" fullscreen-main 1280 720 >"$UMBRIEL_RUNTIME_DIR/fullscreen-main.log" 2>&1 &
wait_for_window fullscreen-main
capture
assert_active fullscreen-main
assert_pixel 300 260 "0 0 255" "the main window did not fill the output"

# Without an explicit focus choice, an unrelated float must not steal fullscreen focus.
env APP_ID=floating-default-focus FILL_COLOR=0xFFFF00FF \
  "$UMBRIEL_UNMAP_CLIENT" default-focus-float 220 220 >"$UMBRIEL_RUNTIME_DIR/default-focus-float.log" 2>&1 &
wait_for_window default-focus-float
capture
assert_active fullscreen-main
assert_pixel 1050 260 "0 0 255" "a default-focus arrival appeared above fullscreen"

# Opening an independent float without focus must not disturb fullscreen stacking.
env APP_ID="$APP_ID" FILL_COLOR=0xFFFF0000 \
  "$UMBRIEL_UNMAP_CLIENT" unfocused-float 300 220 >"$UMBRIEL_RUNTIME_DIR/unfocused-float.log" 2>&1 &
wait_for_window unfocused-float
capture
assert_active fullscreen-main
assert_pixel 300 260 "0 0 255" "an unfocused arrival appeared above fullscreen"

# These are separate native toplevels, matching apps that do not set a transient parent.
env APP_ID="$APP_ID" FILL_COLOR=0xFF00FF00 \
  "$UMBRIEL_UNMAP_CLIENT" focused-float 300 220 >"$UMBRIEL_RUNTIME_DIR/focused-float.log" 2>&1 &
wait_for_window focused-float
capture
assert_active focused-float
windows | jq -e '[.[] | select(.floating and .parent == "")] | length == 3' >/dev/null
assert_pixel 700 260 "0 255 0" "the newly focused float stayed behind fullscreen"
assert_pixel 300 260 "255 0 0" "focusing a float did not reveal the stacking layout"
assert_pixel 1050 260 "255 0 255" "focusing a float did not reveal a default-focus peer"

# Hover changes keyboard focus without burying floats that remain under the pointer's control.
sed -i 's/^follows_mouse = false$/follows_mouse = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 100 600
capture
assert_active fullscreen-main
assert_pixel 700 260 "0 255 0" "hovering fullscreen buried the raised stacking layout"

# A click is explicit focus and must restore fullscreen stacking even after hover focused it.
"$UMBRIEL_POINTER_CLIENT" 1280 720 click 272
capture
assert_active fullscreen-main
assert_pixel 700 260 "0 0 255" "refocusing fullscreen left the focused float above it"
assert_pixel 300 260 "0 0 255" "refocusing fullscreen left the other float above it"
assert_pixel 1050 260 "0 0 255" "refocusing fullscreen left a default-focus peer above it"
sed -i 's/^follows_mouse = true$/follows_mouse = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload >/dev/null

float_id=$(windows | jq -r '.[] | select(.title == "focused-float") | .id')
"$UMBRIEL" msg window-focus "$float_id" >/dev/null
capture
assert_active focused-float
assert_pixel 700 260 "0 255 0" "refocusing the float did not reveal it again"
assert_pixel 300 260 "255 0 0" "refocusing the float did not reveal its floating peers"

# Focusing an ordinary existing float uses the same stacking behavior as an opening focus rule.
default_id=$(windows | jq -r '.[] | select(.title == "default-focus-float") | .id')
"$UMBRIEL" msg window-focus "$default_id" >/dev/null
capture
assert_active default-focus-float
assert_pixel 1050 260 "255 0 255" "explicit focus did not reveal a default-focus float"

# Dragging reparents a float temporarily, but its raised peers must remain visible.
"$UMBRIEL" msg window-focus "$float_id" >/dev/null
pointer_hold 1280 720 move 700 260 mod super press 272 move 760 310 -- release 272 mod none
capture
assert_pixel 760 310 "0 255 0" "dragging the focused float put it behind fullscreen"
assert_pixel 300 260 "255 0 0" "dragging the focused float hid its floating peers"
pointer_release
capture
assert_active focused-float
assert_pixel 760 310 "0 255 0" "releasing the drag put the focused float behind fullscreen"

# The fullscreen action belongs to the visible float, rather than the fullscreen window below it.
"$UMBRIEL" msg window-toggle-fullscreen >/dev/null
capture
assert_active focused-float
if ! "$UMBRIEL" tearing --json | jq -e \
  'any(.surfaces[]; .title == "focused-float" and .fullscreen)' >/dev/null; then
  echo "the fullscreen action did not target the focused float: $(windows)"
  exit 1
fi
"$UMBRIEL" msg window-toggle-fullscreen >/dev/null
capture
assert_active focused-float
assert_pixel 760 310 "0 255 0" "leaving fullscreen did not restore the raised float"

# Removing the raised float must retain a usable main window and floating peers.
"$UMBRIEL" msg window-close >/dev/null
wait_for_absent focused-float
"$UMBRIEL" settle >/dev/null
main_id=$(windows | jq -r '.[] | select(.title == "fullscreen-main") | .id')
"$UMBRIEL" msg window-focus "$main_id" >/dev/null
capture
assert_active fullscreen-main
assert_pixel 300 260 "0 0 255" "closing a raised float left its peers above refocused fullscreen"
"$UMBRIEL" msg window-focus "$default_id" >/dev/null
capture
assert_active default-focus-float
assert_pixel 1050 260 "255 0 255" "the default-focus float did not raise before moving"
"$UMBRIEL" msg window-move-to-workspace-silent 2 >/dev/null
"$UMBRIEL" msg window-focus "$main_id" >/dev/null
capture
assert_active fullscreen-main
assert_pixel 1050 260 "0 0 255" "moving a raised float left stale content over fullscreen"

echo "floating focus reveals independent floats above fullscreen until the fullscreen window is refocused"
