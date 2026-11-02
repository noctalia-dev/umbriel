#!/usr/bin/env bash
# Automatic title bars reserve tile space and preserve client-side decoration preferences.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
title_bar_config

env XDG_DECORATION=server "$UMBRIEL_FRACTIONAL_CLIENT" ssd-float-render 480 300 > "$UMBRIEL_RUNTIME_DIR/ssd.log" 2>&1 &
title_bar_wait_box ssd-float-render 480x300+173+109
title_bar_wait_field ssd-float-render title_bar true
"$UMBRIEL" settle > /dev/null
image=$UMBRIEL_RUNTIME_DIR/title-bar.png
grim -o HEADLESS-1 "$image"
read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel 180 85)
awk -v r="$red" -v g="$green" -v b="$blue" 'BEGIN { exit !(r > 240 && g < 12 && b > 240) }'
read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel 633 95)
awk -v r="$red" -v g="$green" -v b="$blue" 'BEGIN { exit !(g > 200 && r < 64 && b < 64) }'

# Explicit client-side decoration keeps client controls; no preference uses the default.
env XDG_DECORATION=client "$UMBRIEL_FRACTIONAL_CLIENT" ssd-csd-render 300 200 > "$UMBRIEL_RUNTIME_DIR/csd.log" 2>&1 &
title_bar_wait_field ssd-csd-render title_bar false
"$UMBRIEL_FRACTIONAL_CLIENT" ssd-no-protocol 300 200 > "$UMBRIEL_RUNTIME_DIR/no-protocol.log" 2>&1 &
title_bar_wait_field ssd-no-protocol title_bar true

# A tile's outer allocation stays fixed as its content gains or loses the title bar.
env LOG_CONFIGURES=1 XDG_DECORATION=server "$UMBRIEL_FRACTIONAL_CLIENT" ssd-tile-render 480 300 > "$UMBRIEL_RUNTIME_DIR/tile.log" 2>&1 &
title_bar_wait_field ssd-tile-render title_bar true
title_bar_wait_field ssd-csd-render title_bar false
title_bar_wait_field ssd-no-protocol title_bar true
"$UMBRIEL" settle > /dev/null
on=$(title_bar_window ssd-tile-render)
on_h=$(jq -r '.h' <<< "$on")
on_y=$(jq -r '.y' <<< "$on")
sed -i '/^\[appearance.title_bar\]$/ { n; s/mode = "on"/mode = "off"/; }' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
title_bar_wait_field ssd-tile-render title_bar false
title_bar_wait_field ssd-csd-render title_bar false
title_bar_wait_field ssd-no-protocol title_bar false
"$UMBRIEL" settle > /dev/null
off=$(title_bar_window ssd-tile-render)
[[ $(jq -r '.h' <<< "$off") == "$((on_h + 28))" ]]
[[ $(jq -r '.y' <<< "$off") == "$((on_y - 28))" ]]
sed -i '/^\[appearance.title_bar\]$/ { n; s/mode = "off"/mode = "on"/; }' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
title_bar_wait_field ssd-tile-render title_bar true
"$UMBRIEL" settle > /dev/null
[[ $(title_bar_window ssd-tile-render | jq -r '.h') == "$on_h" ]]

# Initial sizing already includes the title bar, before map can arrange it again.
first=$(rg -m 1 '^configure ' "$UMBRIEL_RUNTIME_DIR/tile.log")
[[ ${first##*x} == "$on_h" ]]

# Tab groups replace SSD title bars, and leaving the group restores the frame.
tile_id=$(title_bar_window ssd-tile-render | jq -r '.id')
"$UMBRIEL" msg window-focus "$tile_id" > /dev/null
"$UMBRIEL" msg column-set-display tabbed > /dev/null
title_bar_wait_field ssd-tile-render tabbed true
title_bar_wait_field ssd-tile-render title_bar false
"$UMBRIEL" msg column-set-display normal > /dev/null
title_bar_wait_field ssd-tile-render tabbed false
title_bar_wait_field ssd-tile-render title_bar true

# Every tiling mode keeps content below its title bar when the setting changes.
for mode in dwindle master; do
  "$UMBRIEL" msg workspace-set-layout "$mode" > /dev/null
  "$UMBRIEL" settle > /dev/null
  y=$(title_bar_window ssd-tile-render | jq -r '.y')
  ((y >= 28))
done

# Fullscreen suppresses the title bar; maximize-to-edges and normal maximize retain it.
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
title_bar_wait_field ssd-tile-render title_bar false
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
title_bar_wait_field ssd-tile-render title_bar true
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
title_bar_wait_field ssd-tile-render title_bar true
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
title_bar_wait_field ssd-tile-render title_bar true

# Overview shows a preview of the frame and releases its scene nodes safely.
"$UMBRIEL" msg overview-toggle > /dev/null
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-1 "$image"
count=$("$UMBRIEL_PIXEL_PROBE" "$image" count 'r > 0.8 && g < 0.2 && b > 0.8')
((count > 50))
"$UMBRIEL" msg overview-toggle > /dev/null
"$UMBRIEL" settle > /dev/null

# A close snapshot keeps the frame and fades it with the client, rather than leaving an opaque bar.
float_id=$(title_bar_window ssd-float-render | jq -r '.id')
"$UMBRIEL" msg window-focus "$float_id" > /dev/null
sed -i '/^\[animation\]$/ { n; s/enabled = false/enabled = true/; }' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'
[animation.windows_out]
enabled = true
style = "fade"
duration_ms = 1000
curve = "linear"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" clock-freeze > /dev/null
"$UMBRIEL" msg window-close "$float_id" > /dev/null
title_bar_wait_absent ssd-float-render
grim -o HEADLESS-1 "$image"
read -r red _ _ < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel 180 85)
((red > 240))
"$UMBRIEL" clock-advance 500 > /dev/null
grim -o HEADLESS-1 "$image"
read -r red _ _ < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel 180 85)
((red > 20 && red < 240))
"$UMBRIEL" clock-advance 500 > /dev/null
"$UMBRIEL" clock-resume > /dev/null
sed -i '/^\[animation\]$/ { n; s/enabled = true/enabled = false/; }' "$UMBRIEL_CONFIG"

# Fractional output scale preserves logical frame placement and paints the same title bar color.
cat >> "$UMBRIEL_CONFIG" <<'EOF'
[output."HEADLESS-1"]
scale = 1.25
EOF
"$UMBRIEL" msg config-reload > /dev/null
env XDG_DECORATION=server "$UMBRIEL_FRACTIONAL_CLIENT" ssd-float-scaled 480 300 > "$UMBRIEL_RUNTIME_DIR/scaled.log" 2>&1 &
title_bar_wait_box ssd-float-scaled 480x300+173+109
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-1 "$image"
read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel 225 106)
((red > 240 && green < 12 && blue > 240))
echo "title bar rendering, automatic decoration selection, and tile reservation passed"
