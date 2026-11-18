#!/usr/bin/env bash
# Smooth wheel and finger zoom preserve input dispatch, selection, and spring momentum.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/overview_zoom.sh"
zoom_config
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[keybinds]
"Mod+WheelUp" = "overview-zoom-in"
"Mod+WheelDown" = "overview-zoom-out"
"Mod+WheelLeft" = "overview-zoom-in"
"Mod+WheelRight" = "overview-zoom-out"
"Alt+WheelDown" = "overview-zoom-cycle"
"Mod+Page_Up" = "overview-zoom-in"
"Mod+Page_Down" = "overview-zoom-out"
"Ctrl+WheelDown" = { action = "overview-zoom-out", cooldown_ms = 60000 }
"submap[zoom],Mod+WheelDown" = { action = "overview-zoom-out", submap = "reset" }
TOML
zoom_start 0.5

zoom_action overview-zoom-cycle 100

# Fractional wheel and finger samples move immediately and give the same result as the equivalent whole travel.
for stream in wheel axis; do
  "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo "$stream" vertical 7.5 mod none
  "$UMBRIEL" settle
  zoom_measure "$stream-half" 95
  "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo "$stream" vertical 7.5 mod none
  "$UMBRIEL" settle
  zoom_measure "$stream-two-halves" 91
  "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo "$stream" vertical -15 mod none
  "$UMBRIEL" settle
  zoom_measure "$stream-reversed" 100
done

# Horizontal scroll bindings use the same continuous distance.
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel horizontal 7.5 mod none
"$UMBRIEL" settle
zoom_measure horizontal-out 95
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo axis horizontal -7.5 mod none
"$UMBRIEL" settle
zoom_measure horizontal-in 100

# No latent fractional notch may escape into navigation after the modifier is released.
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo axis vertical 7.5 axis-stop vertical mod none
"$UMBRIEL" settle
zoom_measure finger-stop 95
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo tap 104 mod none
"$UMBRIEL" settle
zoom_measure keyboard-preset-in 100
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo tap 109 mod none
"$UMBRIEL" settle
zoom_measure keyboard-preset-out 40
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo axis-stop vertical mod none
"$UMBRIEL" settle
zoom_measure stop-does-not-zoom 40
zoom_action overview-zoom-in 100

# Continuous input still honors cooldown and one-shot submaps through the regular action dispatcher.
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod control wheel vertical 7.5 wheel vertical 7.5 mod none
"$UMBRIEL" settle
zoom_measure scroll-cooldown 95
"$UMBRIEL" msg submap zoom > /dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo axis-stop vertical mod none
[[ $("$UMBRIEL" submap --json | jq -r .) == zoom ]]
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel vertical 7.5 mod none
"$UMBRIEL" settle
zoom_measure scroll-submap 91
[[ $("$UMBRIEL" submap --json | jq -r .) == null ]]
zoom_action overview-zoom-in 100

# Discrete cycle bindings remain one action per notch, including opening from the desktop.
zoom_action overview-close 400
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod alt notch 1 mod none
"$UMBRIEL" settle
zoom_measure wheel-cycle-open 100
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod alt notch 1 mod none
"$UMBRIEL" settle
zoom_measure wheel-cycle-step 200

zoom_action overview-close 400
sed -i '/^\[animation\]/{n;s/enabled = false/enabled = true/;}' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[animation.overview]
enabled = true
curve = "spring:1,700"
TOML
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
# Repeated scroll events retain spring velocity instead of stopping and restarting the animation.
zoom_action overview-zoom-out 200
"$UMBRIEL" clock-freeze
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel vertical 150 mod none
"$UMBRIEL" clock-advance 60
grim "$ZOOM_IMAGE"
read -r _ _ before_width _ < <("$UMBRIEL_PIXEL_PROBE" "$ZOOM_IMAGE" bbox "$ZOOM_PIXEL_TEST")
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo notch 1 mod none
"$UMBRIEL" clock-advance 10
grim "$ZOOM_IMAGE"
read -r _ _ after_width _ < <("$UMBRIEL_PIXEL_PROBE" "$ZOOM_IMAGE" bbox "$ZOOM_PIXEL_TEST")
if (( before_width - after_width < 6 )); then
  echo "scroll retarget lost velocity: $before_width -> $after_width"
  exit 1
fi
"$UMBRIEL" clock-advance 5000
zoom_measure scrolling-spring-settled 70
"$UMBRIEL" clock-resume

# Sensitivity changes distance without changing keyboard presets.
sed -i '/^zoom = /a zoom_scroll_factor = 0.5' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
zoom_action overview-zoom-out 200
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo notch 1 mod none
"$UMBRIEL" settle
zoom_measure fine-scroll 191
zoom_action overview-zoom-out 100

# Zoom input cannot resize a card or its workspace while a drag owns navigation.
for button in 274 272; do
  pointer_hold 1280 720 move 640 360 press "$button" move 670 360 --
  "$UMBRIEL" msg overview-zoom-out > /dev/null
  "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel vertical 7.5 mod none
  "$UMBRIEL" settle
  zoom_measure "held-$button" 100
  pointer_release
  "$UMBRIEL" settle
done

echo 'continuous overview zoom, dispatch, and spring momentum passed'
