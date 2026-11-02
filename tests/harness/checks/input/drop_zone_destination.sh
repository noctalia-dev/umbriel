#!/usr/bin/env bash
# harness: outputs=2
# Destination policy controls a cross-output drag, and stacking policy windows become tiled in a tiling destination.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[layout]
mode = "stacking"
[layout.stacking]
drop_zones = true
[[workspace]]
output = "HEADLESS-2"
index = 1
layout.stacking.drop_zones = false
[output.HEADLESS-1]
position = [0, 0]
[output.HEADLESS-2]
position = [1280, 0]
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "off"
[colors]
insert_hint = "#00FFFFFF"
[[window_rule]]
match.title = "^destination-independent$"
default_output = "HEADLESS-1"
default_floating = true
default_position = { x = 173, y = 109, anchor = "top_left" }
[[window_rule]]
match.title = "^destination-policy$"
default_output = "HEADLESS-1"
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" destination-independent 480 300 >"$UMBRIEL_RUNTIME_DIR/independent.log" 2>&1 &
title_bar_wait_box destination-independent 480x300+173+109
pointer_hold 2560 720 move 400 240 mod logo press 272 move 2559 360 -- release 272 mod none
"$UMBRIEL" settle >/dev/null
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/destination.png"
hint=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/destination.png" bbox 'r < 0.1 && g > 0.9 && b > 0.9')
[[ $hint == '0 0 0 0' ]] || {
  echo "disabled destination received a drop-zone preview: $hint"
  exit 1
}
pointer_release
title_bar_wait_field destination-independent workspace HEADLESS-2:1
title_bar_wait_field destination-independent region null
title_bar_wait_field destination-independent w 480
title_bar_wait_field destination-independent h 300
"$UMBRIEL" msg window-set-region right >/dev/null
title_bar_wait_field destination-independent region right
title_bar_wait_box destination-independent 628x704+1924+8
"$UMBRIEL" msg window-clear-region >/dev/null
title_bar_wait_field destination-independent region null

"$UMBRIEL" msg workspace-set-layout scrolling >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" destination-policy 480 300 >"$UMBRIEL_RUNTIME_DIR/policy.log" 2>&1 &
title_bar_wait_field destination-policy workspace HEADLESS-1:1
title_bar_wait_field destination-policy floating true
read -r x y < <(title_bar_window destination-policy | jq -r '"\(.x + .w / 2 | floor) \(.y + .h / 2 | floor)"')
"$UMBRIEL_POINTER_CLIENT" 2560 720 move "$x" "$y" mod logo press 272 move 1920 360 release 272 mod none
title_bar_wait_field destination-policy workspace HEADLESS-2:1
title_bar_wait_field destination-policy floating false
title_bar_wait_field destination-policy region null
# Scrolling retains the requested 480 px strip width and centers the underfull strip, then tiles its height.
title_bar_wait_box destination-policy 480x704+1680+8
echo "cross-output drop zones use destination policy and stacking policy floats rejoin a tiling destination"
