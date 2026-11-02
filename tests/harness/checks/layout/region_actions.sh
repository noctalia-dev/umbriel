#!/usr/bin/env bash
# Region actions preserve the original restore box through repeated sets, changes, maximize, and fullscreen.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[layout]
mode = "stacking"
[layout.stacking]
drop_zones = false
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "off"
[[window_rule]]
match.title = "^region-action$"
default_floating = true
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" region-action 480 300 >"$UMBRIEL_RUNTIME_DIR/region-action.log" 2>&1 &
title_bar_wait_box region-action 480x300+173+109
title_bar_window region-action | jq -e 'has("region") and (has("snap") | not)' >/dev/null
set_region() {
  "$UMBRIEL" msg window-set-region "$1" >/dev/null
  title_bar_wait_field region-action region "$1"
}
set_region left
title_bar_wait_box region-action 628x704+8+8
set_region left
title_bar_wait_box region-action 628x704+8+8
set_region right
title_bar_wait_box region-action 628x704+644+8
set_region right
title_bar_wait_box region-action 628x704+644+8
"$UMBRIEL" msg window-clear-region >/dev/null
title_bar_wait_field region-action region null
title_bar_wait_box region-action 480x300+173+109
"$UMBRIEL" msg window-clear-region >/dev/null
title_bar_wait_box region-action 480x300+173+109
set_region bottom_right
title_bar_wait_box region-action 628x348+644+364
"$UMBRIEL" msg window-toggle-maximize >/dev/null
title_bar_wait_box region-action 1264x704+8+8
set_region bottom_right
title_bar_wait_field region-action maximized false
title_bar_wait_box region-action 628x348+644+364
"$UMBRIEL" msg window-toggle-fullscreen >/dev/null
title_bar_wait_box region-action 1280x720+0+0
"$UMBRIEL" msg window-toggle-fullscreen >/dev/null
title_bar_wait_field region-action region bottom_right
title_bar_wait_box region-action 628x348+644+364
"$UMBRIEL" msg window-clear-region >/dev/null
title_bar_wait_box region-action 480x300+173+109
# Maximize-to-edges suppresses the title bar, but region placement must restore it before computing the region.
sed -i 's/mode = "off"/mode = "forced"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" msg window-toggle-maximize-to-edges >/dev/null
set_region top_left
title_bar_wait_field region-action title_bar true
title_bar_wait_box region-action 628x320+8+36
# A manual size adjustment ends region placement instead of being refit on the next arrange.
"$UMBRIEL" msg window-modify-width-right 0.1 >/dev/null
title_bar_wait_field region-action region null
# A fixed-size client larger than a quarter cannot region.
FIXED_SIZE=1 "$UMBRIEL_SUBSURFACE_CLIENT" fixed-region 700 500 >"$UMBRIEL_RUNTIME_DIR/fixed.log" 2>&1 &
title_bar_wait_field fixed-region floating true
"$UMBRIEL" msg window-focus-warp "$(title_bar_window fixed-region | jq -r .id)" >/dev/null
if reply=$("$UMBRIEL" msg window-set-region top_left 2>&1); then
  echo "fixed-size client unexpectedly placed"
  exit 1
fi
[[ $reply == *"cannot fit that region"* ]]
echo "region actions preserve normal geometry and respect client constraints"

# A client can acknowledge the region while retaining its old buffer dimensions. Fullscreen must remember the region
# target, so its exit configures the placed dimensions rather than adopting that stale buffer as the restore size.
"$UMBRIEL" msg workspace-set-layout stacking >/dev/null
HOLD_SIZE=1 LOG_CONFIGURES=1 "$UMBRIEL_FRACTIONAL_CLIENT" pending-region 480 300 >"$UMBRIEL_RUNTIME_DIR/pending.log" 2>&1 &
title_bar_wait_field pending-region floating true
"$UMBRIEL" msg window-set-region left >/dev/null
await_lines "$UMBRIEL_RUNTIME_DIR/pending.log" 'configure 628x676' 1
"$UMBRIEL" settle >/dev/null
before=$(rg -c '^configure 628x676$' "$UMBRIEL_RUNTIME_DIR/pending.log")
"$UMBRIEL" msg window-set-region left >/dev/null
"$UMBRIEL" settle >/dev/null
title_bar_wait_field pending-region region left
after=$(rg -c '^configure 628x676$' "$UMBRIEL_RUNTIME_DIR/pending.log")
[[ $after == "$before" ]] || {
  echo "setting the pending region again issued another size configure"
  exit 1
}
"$UMBRIEL" msg window-toggle-fullscreen >/dev/null
await_lines "$UMBRIEL_RUNTIME_DIR/pending.log" 'configure 1280x720' 1
"$UMBRIEL" settle >/dev/null
"$UMBRIEL" msg window-toggle-fullscreen >/dev/null
await_lines "$UMBRIEL_RUNTIME_DIR/pending.log" 'configure 628x676' $((before + 1))
echo "fullscreen restore uses the placed target while a client retains its old buffer"
