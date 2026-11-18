#!/usr/bin/env bash
# Smooth wheel and finger zoom preserve input dispatch, selection, and spring momentum.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
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
"Mod+WheelLeft" = "overview-zoom-in"
"Mod+WheelRight" = "overview-zoom-out"
"Alt+WheelDown" = "overview-zoom-cycle"
"Mod+Page_Up" = "overview-zoom-in"
"Mod+Page_Down" = "overview-zoom-out"
"Ctrl+WheelDown" = { action = "overview-zoom-out", cooldown_ms = 60000 }
"submap[zoom],Mod+WheelDown" = { action = "overview-zoom-out", submap = "reset" }
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
  read -r x y w h < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.5 && r < 0.1 && g < 0.1')
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
action overview-zoom-cycle 100

# Fractional wheel and finger samples move immediately and give the same result as the equivalent whole travel.
for stream in wheel axis; do
  "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo "$stream" vertical 7.5 mod none
  "$UMBRIEL" settle
  measure "$stream-half" 95
  "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo "$stream" vertical 7.5 mod none
  "$UMBRIEL" settle
  measure "$stream-two-halves" 91
  "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo "$stream" vertical -15 mod none
  "$UMBRIEL" settle
  measure "$stream-reversed" 100
done

# Horizontal scroll bindings use the same continuous distance.
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel horizontal 7.5 mod none
"$UMBRIEL" settle
measure horizontal-out 95
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo axis horizontal -7.5 mod none
"$UMBRIEL" settle
measure horizontal-in 100

# No latent fractional notch may escape into navigation after the modifier is released.
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo axis vertical 7.5 axis-stop vertical mod none
"$UMBRIEL" settle
measure finger-stop 95
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo tap 104 mod none
"$UMBRIEL" settle
measure keyboard-preset-in 100
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo tap 109 mod none
"$UMBRIEL" settle
measure keyboard-preset-out 40
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo axis-stop vertical mod none
"$UMBRIEL" settle
measure stop-does-not-zoom 40
action overview-zoom-in 100

# Continuous input still honors cooldown and one-shot submaps through the regular action dispatcher.
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod control wheel vertical 7.5 wheel vertical 7.5 mod none
"$UMBRIEL" settle
measure scroll-cooldown 95
"$UMBRIEL" msg submap zoom > /dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo axis-stop vertical mod none
[[ $("$UMBRIEL" submap --json | jq -r .) == zoom ]]
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel vertical 7.5 mod none
"$UMBRIEL" settle
measure scroll-submap 91
[[ $("$UMBRIEL" submap --json | jq -r .) == null ]]
action overview-zoom-in 100

# Discrete cycle bindings remain one action per notch, including opening from the desktop.
action overview-close 400
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod alt notch 1 mod none
"$UMBRIEL" settle
measure wheel-cycle-open 100
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod alt notch 1 mod none
"$UMBRIEL" settle
measure wheel-cycle-step 200

# Smooth zoom opens at the largest preset and closes when scrolling beyond it.
action overview-close 400
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel vertical 7.5 mod none
"$UMBRIEL" settle
measure wheel-open 200
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel vertical -7.5 mod none
"$UMBRIEL" settle
measure wheel-close 400

sed -i '/^\[animation\]/{n;s/enabled = false/enabled = true/;}' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[animation.overview]
enabled = true
curve = "spring:1,700"
TOML
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
# Repeated scroll events retain spring velocity instead of stopping and restarting the animation.
action overview-zoom-out 200
"$UMBRIEL" clock-freeze
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel vertical 150 mod none
"$UMBRIEL" clock-advance 60
grim "$IMAGE"
read -r _ _ before_width _ < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.5 && r < 0.1 && g < 0.1')
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo notch 1 mod none
"$UMBRIEL" clock-advance 10
grim "$IMAGE"
read -r _ _ after_width _ < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.5 && r < 0.1 && g < 0.1')
if (( before_width - after_width < 6 )); then
  echo "scroll retarget lost velocity: $before_width -> $after_width"
  exit 1
fi
"$UMBRIEL" clock-advance 5000
measure scrolling-spring-settled 70
"$UMBRIEL" clock-resume

# Sensitivity changes distance without changing keyboard presets.
sed -i '/^zoom = /a zoom_scroll_factor = 0.5' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
action overview-zoom-out 200
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo notch 1 mod none
"$UMBRIEL" settle
measure fine-scroll 191
action overview-zoom-out 100

# Zoom input cannot resize a card or its workspace while a drag owns navigation.
for button in 274 272; do
  pointer_hold 1280 720 move 640 360 press "$button" move 670 360 --
  "$UMBRIEL" msg overview-zoom-out > /dev/null
  "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel vertical 7.5 mod none
  "$UMBRIEL" settle
  measure "held-$button" 100
  pointer_release
  "$UMBRIEL" settle
done

echo 'continuous overview zoom, dispatch, and spring momentum passed'
