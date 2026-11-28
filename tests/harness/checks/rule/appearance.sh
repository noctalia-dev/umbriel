#!/usr/bin/env bash
# Window rules can override title_bar_height independently of the global
# [appearance.title_bar] defaults.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/rule-appearance.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#FFFFFFFF"

[colors.title_bar]
background = "#00FF00FF"
background_unfocused = "#00FF00FF"

[appearance]
border_width = 0
corner_radius = 0

[appearance.title_bar]
mode = "forced"
height = 24

[[window_rule]]
match.title = "^styled-window$"
default_floating = true
default_floating_size_px = { width = 320, height = 220 }
default_position = { x = 140, y = 140, anchor = "top_left" }
title_bar_height = 48
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" styled-window > "$UMBRIEL_RUNTIME_DIR/rule-appearance.log" 2>&1 &
await_window styled-window

WIN=$(window_by_title styled-window)
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-1 "$SCREENSHOT"

wx=$(jq -r '.x' <<< "$WIN")
wy=$(jq -r '.y' <<< "$WIN")
ww=$(jq -r '.w' <<< "$WIN")

# Title bar is 48 px tall (vs global 24 px): sample at wy - 36 (above global 24 px, inside 48 px).
read -r tr tg tb <<< "$("$UMBRIEL_PIXEL_PROBE" "$SCREENSHOT" pixel "$((wx + ww / 2))" "$((wy - 36))")"
if ((tg < 200 || tr > 50 || tb > 50)); then
  echo "expected green 48px overridden title bar at wy-36, got rgb($tr,$tg,$tb)"
  exit 1
fi
