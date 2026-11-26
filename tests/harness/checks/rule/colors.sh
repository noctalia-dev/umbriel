#!/usr/bin/env bash
# Window rules can override title_bar_* colors independently of the global
# [colors.title_bar] defaults.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/rule-colors.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#FFFFFFFF"

[colors.title_bar]
background = "#0000FFFF"
background_unfocused = "#0000FFFF"

[appearance]
border_width = 0
corner_radius = 0

[appearance.title_bar]
mode = "forced"
height = 28

[[window_rule]]
match.title = "^colored-window$"
default_floating = true
default_floating_size_px = { width = 320, height = 220 }
default_position = { x = 120, y = 120, anchor = "top_left" }
title_bar_background = "#00FF00FF"
title_bar_background_unfocused = "#00FF00FF"
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" colored-window > "$UMBRIEL_RUNTIME_DIR/rule-colors.log" 2>&1 &
await_window colored-window

WIN=$(window_by_title colored-window)
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-1 "$SCREENSHOT"

wx=$(jq -r '.x' <<< "$WIN")
wy=$(jq -r '.y' <<< "$WIN")
ww=$(jq -r '.w' <<< "$WIN")

# Title bar sits above the content box (wy - 28 .. wy). Sample its center.
read -r tr tg tb <<< "$("$UMBRIEL_PIXEL_PROBE" "$SCREENSHOT" pixel "$((wx + ww / 2))" "$((wy - 14))")"
if ((tg < 200 || tr > 50 || tb > 50)); then
  echo "expected green overridden title bar, got rgb($tr,$tg,$tb)"
  exit 1
fi
