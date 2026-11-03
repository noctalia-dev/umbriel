#!/usr/bin/env bash
# The title bar's minimize button minimizes a floating window and is absent while the window tiles, and a window
# rule's title_bar_buttons replaces the configured buttons live.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "on"
height = 28
[colors.title_bar]
background = "#FF00FFFF"
background_unfocused = "#FF00FFFF"
close = "#00FF00FF"
maximize = "#FFFF00FF"
minimize = "#0000FFFF"
[[window_rule]]
match.title = "^ssd-buttons$"
default_floating = true
default_floating_size_px = { width = 480, height = 300 }
default_position = { x = 173, y = 109, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

readonly TITLE=ssd-buttons
readonly IMAGE=$UMBRIEL_RUNTIME_DIR/title-bar-buttons.png

window() { "$UMBRIEL" windows --json | jq -c --arg title "$TITLE" '.[] | select(.title == $title)'; }

wait_field() {
  local field=$1 expected=$2 actual=
  for _ in $(seq 80); do
    actual=$(window | jq -r --arg field "$field" '.[$field]')
    [[ $actual == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected $field=$expected, got $actual: $(window)"
  return 1
}

# Sample the background beside each symbol in the slot `offset` pixels left of the bar's right edge.
slot() {
  local x y w red green blue
  read -r x y w < <(window | jq -r '"\(.x) \(.y) \(.w)"')
  grim "$IMAGE"
  read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$((x + w - $1 - 6))" "$((y - 14))")
  if ((red > 200 && green < 60 && blue > 200)); then
    echo bar
  elif ((red < 100 && green < 100 && blue > 200)); then
    echo blue
  elif ((red > 200 && green > 150 && blue < 100)); then
    echo yellow
  elif ((red < 100 && green > 200 && blue < 100)); then
    echo green
  else
    echo "rgb($red,$green,$blue)"
  fi
}

expect_slots() {
  local offset expected actual
  "$UMBRIEL" settle > /dev/null
  for pair in "$@"; do
    offset=${pair%%=*} expected=${pair#*=}
    actual=$(slot "$offset")
    if [[ $actual != "$expected" ]]; then
      echo "expected $expected at $offset px from the right edge, got $actual: $(window)"
      exit 1
    fi
  done
}

env XDG_DECORATION=server "$UMBRIEL_FRACTIONAL_CLIENT" "$TITLE" 480 300 > "$UMBRIEL_RUNTIME_DIR/window.log" 2>&1 &
wait_field floating true
wait_field title_bar true
id=$(window | jq -r .id)

# Default buttons from the title outward: minimize, maximize, close.
expect_slots 14=green 36=yellow 58=blue

# Clicking minimize hides the window; naming it brings it back.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 595 95 click 272
wait_field minimized true
"$UMBRIEL" msg window-focus "$id" > /dev/null
wait_field minimized false

# A tiled window cannot minimize, so its bar has no minimize button.
"$UMBRIEL" msg window-toggle-floating > /dev/null
wait_field floating false
expect_slots 14=green 36=yellow 58=bar

# A rule keeping only close replaces the configured buttons on reload.
cat >> "$UMBRIEL_CONFIG" <<'EOF'
[[window_rule]]
match.title = "^ssd-buttons$"
title_bar_buttons = ["close"]
EOF
"$UMBRIEL" msg config-reload > /dev/null
expect_slots 14=green 36=bar

echo "the minimize button minimizes a floating window, tiled bars omit it, and title_bar_buttons rules apply live"
