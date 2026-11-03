#!/usr/bin/env bash
# Leaving stacking forgets region placement without changing independent float geometry or maximize/fullscreen state.
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
mode = "off"
[[window_rule]]
match.title = "^explicit-region$"
default_floating = true
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" explicit-region 480 300 >"$UMBRIEL_RUNTIME_DIR/explicit.log" 2>&1 &
title_bar_wait_box explicit-region 480x300+173+109
"$UMBRIEL" msg window-set-region bottom_right >/dev/null
title_bar_wait_field explicit-region region bottom_right
title_bar_wait_box explicit-region 628x348+644+364
"$UMBRIEL" msg workspace-set-layout scrolling >/dev/null
title_bar_wait_field explicit-region region null
title_bar_wait_field explicit-region floating true
title_bar_wait_box explicit-region 628x348+644+364
"$UMBRIEL" msg workspace-set-layout stacking >/dev/null
title_bar_wait_field explicit-region region null
title_bar_wait_box explicit-region 628x348+644+364

"$UMBRIEL" msg window-set-region left >/dev/null
title_bar_wait_field explicit-region region left
"$UMBRIEL" msg window-toggle-maximize >/dev/null
title_bar_wait_field explicit-region maximized true
title_bar_wait_box explicit-region 1264x704+8+8
"$UMBRIEL" msg workspace-set-layout dwindle >/dev/null
title_bar_wait_field explicit-region region null
title_bar_wait_field explicit-region maximized true
title_bar_wait_box explicit-region 1264x704+8+8
"$UMBRIEL" msg window-toggle-maximize >/dev/null
title_bar_wait_field explicit-region maximized false
title_bar_wait_box explicit-region 628x704+8+8
"$UMBRIEL" msg workspace-set-layout stacking >/dev/null
title_bar_wait_field explicit-region region null

"$UMBRIEL" msg window-set-region right >/dev/null
title_bar_wait_field explicit-region region right
"$UMBRIEL" msg window-toggle-fullscreen >/dev/null
title_bar_wait_field explicit-region fullscreen true
title_bar_wait_box explicit-region 1280x720+0+0
"$UMBRIEL" msg workspace-set-layout master >/dev/null
title_bar_wait_field explicit-region region null
title_bar_wait_field explicit-region fullscreen true
title_bar_wait_box explicit-region 1280x720+0+0
"$UMBRIEL" msg window-toggle-fullscreen >/dev/null
title_bar_wait_field explicit-region fullscreen false
title_bar_wait_box explicit-region 628x704+644+8
"$UMBRIEL" msg workspace-set-layout stacking >/dev/null
title_bar_wait_field explicit-region region null

"$UMBRIEL_FRACTIONAL_CLIENT" policy-region 480 300 >"$UMBRIEL_RUNTIME_DIR/policy.log" 2>&1 &
title_bar_wait_field policy-region floating true
"$UMBRIEL" msg window-set-region left >/dev/null
title_bar_wait_field policy-region region left
"$UMBRIEL" msg workspace-set-layout scrolling >/dev/null
title_bar_wait_field policy-region floating false
title_bar_wait_field policy-region region null
"$UMBRIEL" msg workspace-set-layout stacking >/dev/null
title_bar_wait_field policy-region floating true
title_bar_wait_field policy-region region null
echo "region cleanup preserves independent floating state and retiles only stacking policy windows"
