#!/usr/bin/env bash
# Scroll zoom boundaries, preset round trips, navigation cancellation, and non-spring tracking.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/overview_zoom.sh"
zoom_config
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[keybinds]
"Mod+WheelUp" = "overview-zoom-in"
"Mod+WheelDown" = "overview-zoom-out"
"Mod+Page_Up" = "overview-zoom-in"
"Mod+Page_Down" = "overview-zoom-out"
"Mod+O" = "overview-zoom-cycle"
"Ctrl+WheelDown" = { action = "overview-zoom-out", cooldown_ms = 60000 }
TOML
zoom_start 0.5

# Round trips can land on either side of a preset by one ulp. The next key must make a full preset step,
# not an invisible adjustment that a pixel tolerance could mistake for success. O cycles in configured order.
while read -r amount key expected; do
  zoom_action overview-close 400
  zoom_action overview-zoom-cycle 100
  "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel vertical "$amount" wheel vertical "-$amount" tap "$key" mod none
  "$UMBRIEL" settle
  zoom_measure "roundtrip-$amount-key-$key" "$expected"
done <<'CASES'
7.96875 109 40
7.96875 24 200
11.25 104 200
CASES
zoom_action overview-zoom-out 100

# Smooth zoom reaches the largest preset before clamping there, even under tiny alternating finger samples.
zoom_action overview-close 400
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel vertical 7.5 mod none
"$UMBRIEL" settle
zoom_measure wheel-open 200
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo wheel vertical 7.5 mod none
"$UMBRIEL" settle
zoom_measure below-largest 191
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo notch -1 mod none
"$UMBRIEL" settle
zoom_measure reaches-largest 200
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo axis vertical -0.0625 axis vertical 0.0625 axis vertical -0.0625 axis-stop vertical mod none
"$UMBRIEL" settle
zoom_measure boundary-jitter 200
zoom_action overview-close 400

# A modifier steals the navigation stream even when zoom cannot change, including a stop-only sample.
for mode in minimum disabled stop cooldown; do
  if [[ $mode == disabled ]]; then
    sed -i 's/^zoom_presets = .*/zoom_presets = []/' "$UMBRIEL_CONFIG"
    "$UMBRIEL" msg config-reload > /dev/null
    zoom_action overview-open 160
    expected_y=300
  else
    if [[ $mode == stop ]]; then
      sed -i 's/^zoom_presets = .*/zoom_presets = [0.25, 0.5, 0.1]/' "$UMBRIEL_CONFIG"
      "$UMBRIEL" msg config-reload > /dev/null
    fi
    zoom_action overview-zoom-out 200
    zoom_action overview-zoom-out 100
    zoom_action overview-zoom-out 40
    expected_y=345
  fi
  sample=(axis vertical 7.5)
  modifier=logo
  [[ $mode != stop ]] || sample=(axis-stop vertical)
  if [[ $mode == cooldown ]]; then
    modifier=control
    # Consume this bind's first event before beginning navigation.
    "$UMBRIEL_POINTER_CLIENT" 1280 720 mod control axis vertical 7.5 mod none
  fi
  pointer_hold 1280 720 move 640 360 axis vertical 80 -- mod "$modifier" "${sample[@]}" mark cancelled hold mod none
  grim "$ZOOM_IMAGE"
  read -r _ shifted_y _ _ < <("$UMBRIEL_PIXEL_PROBE" "$ZOOM_IMAGE" bbox "$ZOOM_PIXEL_TEST")
  (( shifted_y != expected_y ))
  pointer_step cancelled
  "$UMBRIEL" settle
  grim "$ZOOM_IMAGE"
  read -r _ restored_y _ _ < <("$UMBRIEL_PIXEL_PROBE" "$ZOOM_IMAGE" bbox "$ZOOM_PIXEL_TEST")
  if (( restored_y < expected_y - 1 || restored_y > expected_y + 1 )); then
    echo "$mode left finger navigation hanging at y=$restored_y (expected $expected_y)"
    exit 1
  fi
  pointer_release
  zoom_action overview-close 400
done

sed -i '/^\[animation\]/{n;s/enabled = false/enabled = true/;}' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[animation.overview]
enabled = true
curve = "linear"
duration_ms = 1000
TOML
# A stream at about 120 Hz must follow the fingers with non-spring curves, rather than restarting a long ease.
# The clock advances only 8 ms per sample; a 1000 ms animation would remain close to its starting scale.
for curve in linear easeinoutcubic; do
  zoom_action overview-close 400
  sed -i "s/^curve = .*/curve = \"$curve\"/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  zoom_action overview-zoom-out 200
  "$UMBRIEL" clock-freeze
  for _ in $(seq 8); do
    "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo axis vertical 7.5 mod none
    "$UMBRIEL" clock-advance 8
  done
  zoom_measure "$curve-follows-scroll" 137
  "$UMBRIEL" clock-advance 2000
  zoom_measure "$curve-no-trailing-animation" 137

  # Direct scrolling takes over from the rendered quarter-frame of a key-initiated preset step.
  "$UMBRIEL" msg overview-zoom-in > /dev/null
  "$UMBRIEL" clock-advance 1000
  zoom_measure "$curve-before-key-step" 200
  "$UMBRIEL" msg overview-zoom-out > /dev/null
  "$UMBRIEL" clock-advance 250
  if [[ $curve == linear ]]; then
    quarter=175; takeover=167; notch=152
  else
    quarter=194; takeover=185; notch=168
  fi
  zoom_measure "$curve-key-quarter" "$quarter"
  "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo axis vertical 7.5 mod none
  zoom_measure "$curve-scroll-takeover" "$takeover"
  "$UMBRIEL" clock-advance 2000
  zoom_measure "$curve-takeover-settled" "$takeover"
  # Whole wheel notches follow the same immediate input policy for non-spring curves.
  "$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo notch 1 mod none
  zoom_measure "$curve-whole-notch" "$notch"
  "$UMBRIEL" clock-resume
done

echo 'zoom input rounding, boundaries, navigation cancellation, and curve tracking passed'
