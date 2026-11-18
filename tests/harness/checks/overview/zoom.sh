#!/usr/bin/env bash
# Preset actions change rendered card scale, preserve selection, and compose with open/close animations.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/overview_zoom.sh"
zoom_config
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[keybinds]
"Mod+WheelUp" = "overview-zoom-in"
"Mod+WheelDown" = "overview-zoom-out"
TOML
zoom_start 0.9

zoom_action overview-zoom-in 400  # inert on desktop
zoom_action overview-open 160    # configured default need not be a preset
zoom_action overview-zoom-out 100
zoom_action overview-zoom-out 40
zoom_action overview-zoom-out 40 # smallest clamps
zoom_action overview-zoom-in 100
zoom_action overview-zoom-in 200
zoom_action overview-zoom-in 400 # past largest closes
zoom_action overview-zoom-out 200 # auto-open at largest
zoom_action overview-zoom-cycle 40 # cycle uses list order, not size
zoom_action overview-zoom-cycle 100
zoom_action overview-zoom-cycle 200
zoom_action overview-close 400
zoom_action overview-zoom-cycle 100 # auto-open at first

# Bind dispatch must consume modified wheel input without navigating workspaces.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 360 mod logo notch 1 mod none
"$UMBRIEL" settle
zoom_measure wheel-out 91
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo notch -1 mod none
"$UMBRIEL" settle
zoom_measure wheel-in 100
[[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .index') == 1 ]]

# Unmodified wheel navigation still reaches the neighboring workspace at the new scale.
"$UMBRIEL_POINTER_CLIENT" 1280 720 notch 1
"$UMBRIEL" settle
[[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .index') == 2 ]]
"$UMBRIEL_POINTER_CLIENT" 1280 720 notch -1
"$UMBRIEL" settle
zoom_measure navigation-return 100

# Linear animation gives exact midpoints; closing a half-finished zoom must not snap to its target.
zoom_action overview-close 400
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
zoom_measure opening-half 300
"$UMBRIEL" clock-advance 500
zoom_measure opening-end 200
"$UMBRIEL" msg overview-zoom-out > /dev/null
"$UMBRIEL" clock-advance 500
zoom_measure zoom-half 150
"$UMBRIEL" msg overview-close > /dev/null
zoom_measure close-start 150
"$UMBRIEL" clock-advance 500
zoom_measure close-half 275
"$UMBRIEL" clock-advance 500
zoom_measure close-end 400

# A new zoom action can reverse a close, and rapid steps select from the target, not the frame in flight.
"$UMBRIEL" msg overview-zoom-out > /dev/null
"$UMBRIEL" clock-advance 1000
"$UMBRIEL" msg overview-zoom-out > /dev/null
"$UMBRIEL" msg overview-zoom-out > /dev/null
"$UMBRIEL" clock-advance 1000
zoom_measure rapid-out 40
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" clock-advance 500
zoom_measure interrupted-close 220
"$UMBRIEL" msg overview-zoom-out > /dev/null
zoom_measure reopen-start 220
"$UMBRIEL" clock-advance 1000
zoom_measure reopen-end 200
"$UMBRIEL" clock-resume

# The normal spring path must settle both zoom and row animation at each new scale.
zoom_action overview-close 400
sed -i '/^duration_ms = /d;s/^curve = "linear"$/curve = "spring:1,700"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
zoom_action overview-zoom-out 200
zoom_action overview-zoom-out 100
zoom_action overview-zoom-out 40
zoom_action overview-zoom-in 100
zoom_action overview-zoom-in 200
zoom_action overview-zoom-in 400

# Empty presets disable the actions; ordinary overview still opens at its configured scale.
sed -i 's/^zoom_presets = .*/zoom_presets = []/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
zoom_action overview-zoom-out 400
zoom_action overview-open 160
zoom_action overview-zoom-in 160
zoom_action overview-zoom-cycle 160

# Changed overview settings invalidate the live presentation; reopening uses the new default and preset range.
sed -i 's/^zoom = .*/zoom = 0.3/;s/^zoom_presets = .*/zoom_presets = [0.2, 0.1]/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
zoom_measure reload-closes-overview 400
zoom_action overview-open 120
zoom_action overview-zoom-out 80
zoom_action overview-close 400
zoom_action overview-zoom-out 80
zoom_action overview-zoom-in 400

echo 'overview zoom presets, wheel binds, animation interruption, and selection passed'
