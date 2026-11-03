#!/usr/bin/env bash
# A placed frame follows logical usable-area changes, including panels and title bar reloads.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[layout]
mode = "stacking"
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "forced"
height = 28
[output.HEADLESS-1]
scale = 1.25
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
for _ in $(seq 120); do
  width=$("$UMBRIEL" outputs --json | jq -r '.[0].usable_area.width')
  [[ $width == 1024 ]] && break
  sleep 0.025
done
"$UMBRIEL_FRACTIONAL_CLIENT" refit-region 480 300 >"$UMBRIEL_RUNTIME_DIR/refit-region.log" 2>&1 &
title_bar_wait_field refit-region floating true
"$UMBRIEL" msg window-set-region left >/dev/null
title_bar_wait_box refit-region 500x532+8+36
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 >"$UMBRIEL_RUNTIME_DIR/panel.log" 2>&1 &
panel_pid=$!
await_lines "$UMBRIEL_RUNTIME_DIR/panel.log" ready 1
title_bar_wait_box refit-region 500x492+8+76
sed -i 's/mode = "forced"/mode = "off"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload >/dev/null
title_bar_wait_box refit-region 500x520+8+48
kill "$panel_pid"
wait "$panel_pid" || true
title_bar_wait_box refit-region 500x560+8+8
title_bar_wait_field refit-region region left
"$UMBRIEL" msg window-clear-region >/dev/null
title_bar_wait_field refit-region region null
title_bar_wait_field refit-region w 480
title_bar_wait_field refit-region h 300
echo "placed windows follow scaled outputs, exclusive panels, and title bar geometry"
