#!/usr/bin/env bash
# harness: outputs=2
# Region placement on another output fits its logical geometry and restores a reachable normal box there.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[layout]
mode = "stacking"
[layout.stacking]
drop_zones = true
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "forced"
height = 28
[output.HEADLESS-1]
position = [0, 0]
[output.HEADLESS-2]
position = [1280, 0]
scale = 1.25
[[window_rule]]
match.title = "^output-region$"
default_output = "HEADLESS-1"
default_floating = true
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
for _ in $(seq 120); do
  width=$("$UMBRIEL" outputs --json | jq -r '.[] | select(.name == "HEADLESS-2") | .usable_area.width')
  [[ $width == 1024 ]] && break
  sleep 0.025
done
"$UMBRIEL_FRACTIONAL_CLIENT" output-region 480 300 >"$UMBRIEL_RUNTIME_DIR/output-region.log" 2>&1 &
title_bar_wait_box output-region 480x300+173+109
"$UMBRIEL_POINTER_CLIENT" 2304 720 move 400 95 press 272 move 1800 200 move 2303 250 release 272
title_bar_wait_field output-region workspace HEADLESS-2:1
title_bar_wait_field output-region region right
title_bar_wait_box output-region 500x532+1796+36
"$UMBRIEL" msg window-clear-region >/dev/null
title_bar_wait_field output-region region null
title_bar_wait_box output-region 480x300+1418+87
echo "mixed-scale region transfers preserve a normal restore box on the destination output"
