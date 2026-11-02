#!/usr/bin/env bash
# An oversized client fits its complete frame, and client-driven size changes retain automatic centering.
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
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
mkfifo "$UMBRIEL_RUNTIME_DIR/size-control"
exec 9<>"$UMBRIEL_RUNTIME_DIR/size-control"
DECORATION_CONTROL=1 "$UMBRIEL_FRACTIONAL_CLIENT" automatic-size 1600 900 <&9 >"$UMBRIEL_RUNTIME_DIR/automatic-size.log" 2>&1 &
title_bar_wait_field automatic-size floating true
title_bar_wait_box automatic-size 1201x676+39+36
printf 'resize 640 360\n' >&9
title_bar_wait_box automatic-size 640x360+320+194
# Moving the window takes ownership of its position, so a later client resize must not re-center it.
"$UMBRIEL" msg window-move-to top_left 100 100 >/dev/null
title_bar_wait_box automatic-size 640x360+100+100
printf 'resize 500 300\n' >&9
title_bar_wait_box automatic-size 500x300+100+100
echo "large and client-resized floats fit and center without overriding manual placement"
