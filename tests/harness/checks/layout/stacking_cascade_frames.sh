#!/usr/bin/env bash
# Mixed-size windows preserve exposed frames, including during overlapping opening animations and at fractional scale.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[layout]
mode = "stacking"
[animation]
duration_ms = 2000
curve = "linear"
[appearance]
border_width = 2
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "forced"
height = 28
[[window_rule]]
match.title = "^cascade-explicit$"
default_position = { x = 40, y = 100, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" clock-freeze >/dev/null
for height in 28 60; do
  width=1280
  output_height=720
  if [[ $height == 60 ]]; then
    sed -i 's/^height = 28$/height = 60/' "$UMBRIEL_CONFIG"
    cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[output."HEADLESS-1"]
scale = 1.25
CONFIG
    "$UMBRIEL" msg config-reload >/dev/null
    width=1024
    output_height=576
  fi
  for _ in $(seq 120); do
    actual_width=$("$UMBRIEL" outputs --json | jq -r '.[0].usable_area.width')
    [[ $actual_width == "$width" ]] && break
    sleep 0.025
  done
  [[ $actual_width == "$width" ]] || {
    echo "logical usable width is $actual_width, expected $width"
    exit 1
  }
  small_x=$(((width - 160) / 2))
  small_y=$(((output_height - 100 - height) / 2 + height))
  step=$((height + 2 + 5))
  big_x=$((small_x + step))
  big_y=$((small_y + step))
  "$UMBRIEL_FRACTIONAL_CLIENT" cascade-small 160 100 >"$UMBRIEL_RUNTIME_DIR/small-$height.log" 2>&1 &
  small=$!
  title_bar_wait_box cascade-small "160x100+$small_x+$small_y"
  # The opening animation is still frozen. Use its intended complete frame rather than its tiny drawn frame.
  "$UMBRIEL_FRACTIONAL_CLIENT" cascade-big 320 180 >"$UMBRIEL_RUNTIME_DIR/big-$height.log" 2>&1 &
  big=$!
  title_bar_wait_box cascade-big "320x180+$big_x+$big_y"
  "$UMBRIEL" clock-advance 3000 >/dev/null
  "$UMBRIEL" settle >/dev/null
  # Explicit positions stay authoritative even when they overlap an existing frame.
  "$UMBRIEL_FRACTIONAL_CLIENT" cascade-explicit 320 180 >"$UMBRIEL_RUNTIME_DIR/explicit-$height.log" 2>&1 &
  explicit=$!
  title_bar_wait_box cascade-explicit 320x180+40+100
  "$UMBRIEL" clock-advance 3000 >/dev/null
  "$UMBRIEL" settle >/dev/null
  kill "$small" "$big" "$explicit"
  title_bar_wait_absent cascade-small
  title_bar_wait_absent cascade-big
  title_bar_wait_absent cascade-explicit
  "$UMBRIEL" clock-advance 3000 >/dev/null
  "$UMBRIEL" settle >/dev/null
done
"$UMBRIEL" clock-resume >/dev/null
echo "mixed-size opening frames cascade with titlebar-aware spacing at normal and fractional scale"
