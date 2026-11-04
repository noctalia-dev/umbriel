#!/usr/bin/env bash
# harness: outputs=2
# A layer dims content only on its assigned output and remains bright itself.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/layer-dim.png"
readonly OTHER="$UMBRIEL_RUNTIME_DIR/layer-other.png"
capture_config
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[colors]
backdrop = "#FFFFFFFF"
[output.HEADLESS-1]
position = [0, 0]
[output.HEADLESS-2]
position = [1280, 0]
[[window_rule]]
default_output = "HEADLESS-2"
default_floating = true
default_floating_size_px = { width = 200, height = 180 }
default_position = { x = 500, y = 200, anchor = "top_left" }
[[layer_rule]]
match.namespace = "^umbriel-output-restore-regression$"
match.layer = "overlay"
dim_behind = 0.5
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFF00FF00 FOLLOW_CONFIGURES=1 "$UMBRIEL_UNMAP_CLIENT" under-layer 200 180 > "$UMBRIEL_RUNTIME_DIR/window.log" 2>&1 &
await_window under-layer
"$UMBRIEL_LAYER_CLIENT" HEADLESS-2 0 overlay-layer log-configures > "$UMBRIEL_RUNTIME_DIR/layer.log" 2>&1 &
layer=$!
await_lines "$UMBRIEL_RUNTIME_DIR/layer.log" 'configured-size=' 1 0.025
shot() { "$UMBRIEL" settle; grim -o HEADLESS-2 "$FRAME"; grim -o HEADLESS-1 "$OTHER"; }
shot
assert_region_rgb "$FRAME" 10x10+100+100 255 0 0
assert_region_rgb "$FRAME" 10x10+550+250 0 128 0
assert_region_rgb "$FRAME" 10x10+1200+650 128 128 128
assert_region_rgb "$OTHER" 10x10+1200+650 255 255 255
sed -i 's/dim_behind = 0.5/dim_behind = 0.25/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
shot
assert_region_rgb "$FRAME" 10x10+1200+650 191 191 191
kill "$layer"
for _ in $(seq 100); do
  [[ $("$UMBRIEL" layers --json | jq length) == 0 ]] && break
  sleep 0.025
done
shot
assert_region_rgb "$FRAME" 10x10+550+250 0 255 0
assert_region_rgb "$FRAME" 10x10+1200+650 255 255 255
assert_region_rgb "$OTHER" 10x10+1200+650 255 255 255
echo 'layer dim keeps its owner bright, reloads, clears and stays on its output'
