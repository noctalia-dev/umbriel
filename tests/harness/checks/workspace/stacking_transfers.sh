#!/usr/bin/env bash
# harness: outputs=2
# Both ordinary moves and workspace swaps preserve the reason a window floats.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.title_bar]
mode = "forced"
[[window_rule]]
match.title = "^explicit$"
default_floating = true
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" msg workspace-set-layout stacking >/dev/null
spawn() {
  "$UMBRIEL_FRACTIONAL_CLIENT" "$1" 480 300 >"$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  title_bar_wait_field "$1" floating true
}
focus() { "$UMBRIEL" msg window-focus-warp "$(title_bar_window "$1" | jq -r .id)" >/dev/null; }
spawn policy
spawn explicit
focus policy
"$UMBRIEL" msg window-move-to-workspace 2 >/dev/null
title_bar_wait_field policy floating false
"$UMBRIEL" msg window-move-to-workspace 1 >/dev/null
title_bar_wait_field policy floating true
# A whole-column transfer uses the same floating destination policy.
"$UMBRIEL" msg workspace-switch 2 >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" tiled 480 300 >"$UMBRIEL_RUNTIME_DIR/tiled.log" 2>&1 &
title_bar_wait_field tiled floating false
"$UMBRIEL" msg column-move-to-workspace 1 >/dev/null
title_bar_wait_field tiled floating true
# Swapping layouts' contents must retile policy floats but keep explicit floats.
"$UMBRIEL" msg workspace-switch 1 >/dev/null
focus policy
"$UMBRIEL" msg workspace-swap-active-output-next >/dev/null
title_bar_wait_field policy floating false
title_bar_wait_field tiled floating false
title_bar_wait_field explicit floating true
"$UMBRIEL" msg workspace-swap-active-output-next >/dev/null
title_bar_wait_field policy floating true
title_bar_wait_field tiled floating true
title_bar_wait_field explicit floating true
"$UMBRIEL" msg workspace-set-layout dwindle >/dev/null
title_bar_wait_field policy floating false
title_bar_wait_field tiled floating false
title_bar_wait_field explicit floating true
echo "stacking layout workspace and column transfers verified"
