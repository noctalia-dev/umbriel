#!/usr/bin/env bash
# Preset actions change rendered card scale, preserve selection, and compose with open/close animations.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/zoom.png"
cat >> "$UMBRIEL_CONFIG" <<'TOML'

[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
[overview]
zoom = 0.4
zoom_presets = [0.25, 0.5, 0.1]
shortcuts = false
background_blur = false
[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"
[keybinds]
"Mod+WheelUp" = "overview-zoom-in"
"Mod+WheelDown" = "overview-zoom-out"
[[window_rule]]
match.title = "^overview-zoom$"
default_floating = true
default_position = { x = 440, y = 210, anchor = "top_left" }
TOML
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" overview-zoom 400 300 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq length) == 1 ]] && break
  sleep 0.05
done
[[ $("$UMBRIEL" windows --json | jq length) == 1 ]]
"$UMBRIEL" settle
selection=$("$UMBRIEL" windows --json | jq -r '.[] | select(.focused) | .id')

measure() {
  local label=$1 expected=$2 x y w h
  grim "$IMAGE"
  read -r x y w h < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.9 && r < 0.1 && g < 0.1')
  if (( w < expected - 2 || w > expected + 2 )); then
    echo "$label: card width $w, expected $expected"
    exit 1
  fi
  [[ $("$UMBRIEL" windows --json | jq -r '.[] | select(.focused) | .id') == "$selection" ]]
  echo "$label: width=$w"
}
action() {
  "$UMBRIEL" msg "$1" > /dev/null
  "$UMBRIEL" settle
  measure "$1" "$2"
}
action overview-zoom-in 400  # inert on desktop
action overview-open 160    # configured default need not be a preset
action overview-zoom-out 100
action overview-zoom-out 40
action overview-zoom-out 40 # smallest clamps
action overview-zoom-in 100
action overview-zoom-in 200
action overview-zoom-in 400 # past largest closes
action overview-zoom-out 200 # auto-open at largest
action overview-zoom-cycle 40 # cycle uses list order, not size
action overview-zoom-cycle 100
action overview-zoom-cycle 200
action overview-close 400
action overview-zoom-cycle 100 # auto-open at first

# Bind dispatch must consume modified wheel input without navigating workspaces.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 360 mod logo notch 1 mod none
"$UMBRIEL" settle
measure wheel-out 40
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo notch -1 mod none
"$UMBRIEL" settle
measure wheel-in 100
[[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .index') == 1 ]]

# Unmodified wheel navigation still reaches the neighboring workspace at the new scale.
"$UMBRIEL_POINTER_CLIENT" 1280 720 notch 1
"$UMBRIEL" settle
[[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .index') == 2 ]]
"$UMBRIEL_POINTER_CLIENT" 1280 720 notch -1
"$UMBRIEL" settle
measure navigation-return 100

# Linear animation gives exact midpoints; closing a half-finished zoom must not snap to its target.
action overview-close 400
sed -i '/^\[animation\]/{n;s/enabled = false/enabled = true/;}' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'TOML'

[animation.overview]
enabled = true
duration_ms = 1000
curve = "linear"
TOML
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg overview-zoom-out > /dev/null
"$UMBRIEL" clock-advance 500
measure opening-half 300
"$UMBRIEL" clock-advance 500
measure opening-end 200
"$UMBRIEL" msg overview-zoom-out > /dev/null
"$UMBRIEL" clock-advance 500
measure zoom-half 150
"$UMBRIEL" msg overview-close > /dev/null
measure close-start 150
"$UMBRIEL" clock-advance 500
measure close-half 275
"$UMBRIEL" clock-advance 500
measure close-end 400

# A new zoom action can reverse a close, and rapid steps select from the target, not the frame in flight.
"$UMBRIEL" msg overview-zoom-out > /dev/null
"$UMBRIEL" clock-advance 1000
"$UMBRIEL" msg overview-zoom-out > /dev/null
"$UMBRIEL" msg overview-zoom-out > /dev/null
"$UMBRIEL" clock-advance 1000
measure rapid-out 40
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" clock-advance 500
measure interrupted-close 220
"$UMBRIEL" msg overview-zoom-out > /dev/null
measure reopen-start 220
"$UMBRIEL" clock-advance 1000
measure reopen-end 200
"$UMBRIEL" clock-resume

# The normal spring path must settle both zoom and row animation at each new scale.
action overview-close 400
sed -i '/^duration_ms = /d;s/^curve = "linear"$/curve = "spring:1,700"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
action overview-zoom-out 200
action overview-zoom-out 100
action overview-zoom-out 40
action overview-zoom-in 100
action overview-zoom-in 200
action overview-zoom-in 400

# Empty presets disable the actions; ordinary overview still opens at its configured scale.
sed -i 's/^zoom_presets = .*/zoom_presets = []/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
action overview-zoom-out 400
action overview-open 160
action overview-zoom-in 160
action overview-zoom-cycle 160

echo 'overview zoom presets, wheel binds, animation interruption, and selection passed'
