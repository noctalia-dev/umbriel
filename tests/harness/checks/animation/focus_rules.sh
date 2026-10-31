#!/usr/bin/env bash
# Window opacity and blur use their own focus timeline in both directions. Interrupted transitions start from their
# drawn appearance, and disabling opacity animation preserves immediate focus changes.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
msg() { "$UMBRIEL" msg "$@" > /dev/null; }
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/focus.png"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = true
curve = "linear"
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
[animation.windows_move]
enabled = false
[animation.windows_opacity]
duration_ms = 1000
curve = "linear"
[animation.border]
enabled = false
[animation.dim_unfocused]
enabled = false
[appearance]
border_width = 8
outer_border_width = 4
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "forced"
height = 28
[colors.title_bar]
background = "#00000000"
background_unfocused = "#00000000"
[appearance.blur]
enabled = true
optimized = false
passes = 2
radius = 6
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0

[[window_rule]]
match.title = "^focus-"
default_floating = true
default_floating_size_px = { width = 400, height = 240 }
[[window_rule]]
match.title = "^focus-blur$"
default_position = { x = 100, y = 128, anchor = "top_left" }
[[window_rule]]
match.title = "^focus-opacity$"
default_position = { x = 700, y = 128, anchor = "top_left" }
[[window_rule]]
match.title = "^focus-home$"
default_position = { x = 100, y = 450, anchor = "top_left" }

[[window_rule]]
match.title = "^focus-blur$"
match.is_focused = false
blur = true
[[window_rule]]
match.title = "^focus-blur$"
match.is_focused = true
blur = false
[[window_rule]]
match.title = "^focus-opacity$"
match.is_focused = false
opacity = 0.3
border_color_unfocused = "#0000FFFF"
border_color_outer = "#FF0000FF"
[[window_rule]]
match.title = "^focus-opacity$"
match.is_focused = true
opacity = 1.0
border_color_focused = "#FF0000FF"
border_color_outer = "#00FF00FF"
CONFIG
msg config-reload
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 > "$UMBRIEL_RUNTIME_DIR/background.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/background.log" ready 1
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 bottom-layer > "$UMBRIEL_RUNTIME_DIR/square.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/square.log" ready 1
for name in blur opacity home; do
  color=0xFF000000
  if [[ $name == blur ]]; then color=0x00000000; fi
  FILL_COLOR="$color" "$UMBRIEL_UNMAP_CLIENT" "focus-$name" 400 240 > "$UMBRIEL_RUNTIME_DIR/$name.log" 2>&1 &
  await_window "focus-$name"
done
"$UMBRIEL" settle
blur=$(window_by_title focus-blur | jq -r .id)
opacity=$(window_by_title focus-opacity | jq -r .id)
home=$(window_by_title focus-home | jq -r .id)
ox=$(window_by_title focus-opacity | jq -r .x)
oy=$(window_by_title focus-opacity | jq -r .y)
"$UMBRIEL" clock-freeze

pixel() {
  local x=$1 y=$2 channel=$3
  "$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y" | awk -v channel="$channel" '{ print $channel }'
}
sample() {
  grim -o HEADLESS-1 "$IMAGE"
  case $1 in
    blur) pixel 202 150 2 ;;
    title) pixel 202 114 2 ;;
    opacity) pixel "$((ox + 100))" "$((oy + 100))" 2 ;;
  esac
}
between() {
  local value=$1 first=$2 last=$3 label=$4
  if ! (( (value > first + 2 && value < last - 2) || (value < first - 2 && value > last + 2) )); then
    echo "$label: expected an intermediate pixel between $first and $last, got $value"
    exit 1
  fi
}

for property in blur title opacity; do
  target=$blur
  if [[ $property == opacity ]]; then target=$opacity; fi
  msg window-focus "$target"
  "$UMBRIEL" clock-advance 1000 > /dev/null
  focused=$(sample "$property")
  msg window-focus "$home"
  "$UMBRIEL" clock-advance 1000 > /dev/null
  unfocused=$(sample "$property")
  (( focused != unfocused )) || { echo "$property: focus had no visible effect ($focused)"; exit 1; }

  msg window-focus "$target"
  before=$(sample "$property")
  (( before >= unfocused - 1 && before <= unfocused + 1 )) \
    || { echo "$property: focusing snapped from $unfocused to $before"; exit 1; }
  "$UMBRIEL" clock-advance 500 > /dev/null
  halfway=$(sample "$property")
  between "$halfway" "$focused" "$unfocused" "$property gaining focus"
  # Repeated focus requests leave the current transition's clock intact.
  msg window-focus "$target"
  "$UMBRIEL" clock-advance 250 > /dev/null
  later=$(sample "$property")
  between "$later" "$focused" "$halfway" "$property repeated focus"

  msg window-focus "$home"
  reversed=$(sample "$property")
  (( reversed >= later - 1 && reversed <= later + 1 )) \
    || { echo "$property: reversing snapped from $later to $reversed"; exit 1; }
  "$UMBRIEL" clock-advance 500 > /dev/null
  between "$(sample "$property")" "$later" "$unfocused" "$property losing focus"
  "$UMBRIEL" clock-advance 500 > /dev/null
done

# Disabling the table or the master switch both retain immediate rule changes.
for mode in disabled master; do
  if [[ $mode == master ]]; then
    sed -i '/^\[animation.windows_opacity\]$/,/^curve = /s/^enabled = false$/enabled = true/' "$UMBRIEL_CONFIG"
    msg config-reload
  fi
  msg window-focus "$opacity"
  "$UMBRIEL" clock-advance 250 > /dev/null
  between "$(sample opacity)" 0 83 "$mode before disabling a running fade"
  if [[ $mode == disabled ]]; then
    sed -i '/^\[animation.windows_opacity\]$/a enabled = false' "$UMBRIEL_CONFIG"
  else
    sed -i '/^\[animation\]$/,/^curve = /s/^enabled = true$/enabled = false/' "$UMBRIEL_CONFIG"
  fi
  msg config-reload
  msg window-focus "$opacity"
  (( $(sample opacity) <= 1 )) || { echo "$mode: focus opacity was delayed"; exit 1; }
  msg window-focus "$home"
  (( $(sample opacity) > 50 )) || { echo "$mode: unfocused opacity was delayed"; exit 1; }
done

# The per-event spring overrides the shared linear curve and derives its own duration.
sed -i '/^\[animation\]$/,/^curve = /s/^enabled = false$/enabled = true/' "$UMBRIEL_CONFIG"
sed -i '/^\[animation.windows_opacity\]$/,/^curve = /{ /duration_ms = /d; s/curve = "linear"/curve = "spring:1,900"/; }' "$UMBRIEL_CONFIG"
msg config-reload
msg window-focus "$opacity"
"$UMBRIEL" clock-advance 2000 > /dev/null
focused=$(sample opacity)
msg window-focus "$home"
"$UMBRIEL" clock-advance 2000 > /dev/null
unfocused=$(sample opacity)
msg window-focus "$opacity"
"$UMBRIEL" clock-advance 50 > /dev/null
spring=$(sample opacity)
travel=$((unfocused - spring))
distance=$((unfocused - focused))
if ! (( travel * 100 > distance * 30 && travel * 100 < distance * 65 )); then
  echo "per-event spring did not reach the drawn opacity: focused=$focused unfocused=$unfocused at-50ms=$spring"
  exit 1
fi
msg window-focus "$home"
reversed=$(sample opacity)
(( reversed >= spring - 1 && reversed <= spring + 1 )) \
  || { echo "spring reversal snapped from $spring to $reversed"; exit 1; }
"$UMBRIEL" clock-advance 2000 > /dev/null
(( $(sample opacity) > 50 )) || { echo "reversed spring did not settle"; exit 1; }
echo "window opacity and blur use independent timing, reverse continuously, and can change immediately"
