#!/usr/bin/env bash
# Stacking workspaces commit held pointer positions without changing configured drag opacity or other layouts.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
duration_ms = 2000
curve = "linear"
[animation.windows_in]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
drag_opacity = 0.25
[appearance.title_bar]
mode = "forced"
[appearance.shadow]
enabled = false
[colors.title_bar]
background = "#FF0000FF"
background_unfocused = "#FF0000FF"
text = "#FF0000FF"
text_unfocused = "#FF0000FF"
close = "#FF0000FF"
maximize = "#FF0000FF"
[[window_rule]]
match.title = "^floating-live$"
default_floating = true
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" msg workspace-set-layout stacking >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" floating-live 480 300 >"$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
title_bar_wait_box floating-live 480x300+173+109
"$UMBRIEL" settle >/dev/null
"$UMBRIEL" clock-freeze >/dev/null
# Empty title bar space starts a modifier-free move. Observe two positions while the button remains held.
pointer_hold 1280 720 move 300 95 press 272 move 600 275 -- move 650 300 mark moved hold release 272
title_bar_wait_box floating-live 480x300+473+289
grim "$UMBRIEL_RUNTIME_DIR/held.png"
read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/held.png" pixel 480 275)
((red >= 60 && red <= 70 && green < 10 && blue < 10)) || {
  echo "floating drag opacity changed: $red $green $blue"
  exit 1
}
pointer_step moved
title_bar_wait_box floating-live 480x300+523+314
# Maximize while the drag is held, then restore after release. The normal box must be the latest held position.
"$UMBRIEL" msg window-toggle-maximize >/dev/null
"$UMBRIEL" clock-advance 3000 >/dev/null
pointer_release
title_bar_wait_box floating-live 1264x676+8+36
"$UMBRIEL" msg window-toggle-maximize >/dev/null
"$UMBRIEL" clock-advance 3000 >/dev/null
"$UMBRIEL" settle >/dev/null
title_bar_wait_box floating-live 480x300+523+314
# A regular floating window in a scrolling workspace keeps its configured drag opacity.
"$UMBRIEL" msg workspace-set-layout scrolling >/dev/null
"$UMBRIEL" clock-advance 3000 >/dev/null
"$UMBRIEL" settle >/dev/null
title_bar_wait_box floating-live 480x300+523+314
pointer_hold 1280 720 move 650 300 press 272 move 750 350 -- release 272
title_bar_wait_box floating-live 480x300+623+364
grim "$UMBRIEL_RUNTIME_DIR/tiled-layout-held.png"
read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/tiled-layout-held.png" pixel 630 350)
((red >= 60 && red <= 70 && green < 10 && blue < 10)) || {
  echo "scrolling drag opacity changed: $red $green $blue"
  exit 1
}
pointer_release
title_bar_wait_box floating-live 480x300+623+364
"$UMBRIEL" clock-resume >/dev/null
echo "stacking workspace drags commit live position and both layouts retain configured drag opacity"
