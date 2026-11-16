#!/usr/bin/env bash
# A bar keeps optimized blur, but its translucent popup opens over a window and must blur that window. The cached
# wallpaper blur would show the wallpaper through the window instead, as a tooltip's opening fade did.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/optimized-blur-popup.png"
readonly BAR_LOG="$UMBRIEL_RUNTIME_DIR/bar.log"
readonly TITLE=optimized-blur-popup

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = true
optimized = true
passes = 2
radius = 4
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0
[[layer_rule]]
match.namespace = "^umbriel-output-restore-regression$"
match.layer = "top"
blur = true
blur_popups = true
blur_ignore_alpha = 0.4
blur_optimized = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 > "$UMBRIEL_RUNTIME_DIR/wallpaper.log" 2>&1 &
await_events "$UMBRIEL_RUNTIME_DIR/wallpaper.log" ready 1 "the wallpaper"
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 popup-on-release popup-color=0x80000000 > "$BAR_LOG" 2>&1 &
await_events "$BAR_LOG" ready 1 "the bar"
FILL_COLOR=0xFFFF0000 "$UMBRIEL_UNMAP_CLIENT" "$TITLE" 1200 600 > "$UMBRIEL_RUNTIME_DIR/window.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/window.log" mapped 1
"$UMBRIEL" settle

# The 80x80 popup opens one pixel below the click and reaches past the bar over the red window.
x=$(window_by_title "$TITLE" | jq -r '.x + 100')
"$UMBRIEL_POINTER_CLIENT" 1280 720 move "$x" 30 click 272 > /dev/null
await_events "$BAR_LOG" popup-mapped 1 "the popup"
"$UMBRIEL" settle
grim "$IMAGE"
# Half-transparent black over the blurred red window, clear of the window's top edge and the blur's falloff.
assert_region_rgb "$IMAGE" "40x20+$((x + 20))+80" 127 0 0 "the popup over the window"

echo "a bar popup blurs the window below it while the bar keeps optimized blur"
