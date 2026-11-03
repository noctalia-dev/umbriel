#!/usr/bin/env bash
# Floating drags preview halves and quarters, then restore normal size on dragging away.
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
corner_radius = 0
[appearance.title_bar]
mode = "off"
[colors]
insert_hint = "#00FFFFFF"
[[window_rule]]
match.title = "^region-drag$"
default_floating = true
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" region-drag 480 300 >"$UMBRIEL_RUNTIME_DIR/region-drag.log" 2>&1 &
title_bar_wait_box region-drag 480x300+173+109
pointer_hold 1280 720 move 400 240 mod logo press 272 move 2 360 -- release 272 mod none
for _ in $(seq 60); do
  grim "$UMBRIEL_RUNTIME_DIR/preview.png"
  bounds=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/preview.png" bbox 'r < 0.1 && g > 0.9 && b > 0.9')
  [[ $bounds == '8 8 628 704' ]] && break
  sleep 0.025
done
[[ $bounds == '8 8 628 704' ]] || {
  echo "wrong half preview: $bounds"
  exit 1
}
pointer_release
title_bar_wait_field region-drag region left
title_bar_wait_box region-drag 628x704+8+8
# Dropping into the same region again preserves placement and the first normal restore box.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 300 200 mod logo press 272 move 2 360 release 272 mod none
title_bar_wait_field region-drag region left
title_bar_wait_box region-drag 628x704+8+8
"$UMBRIEL" msg window-clear-region >/dev/null
title_bar_wait_box region-drag 480x300+173+109
"$UMBRIEL" msg window-set-region left >/dev/null
title_bar_wait_field region-drag region left
title_bar_wait_box region-drag 628x704+8+8
# A click below the threshold keeps the region. Moving restores the preferred size under the grip.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 300 200 mod logo click 272 mod none
title_bar_wait_field region-drag region left
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 300 200 mod logo press 272 move 400 250 release 272 mod none
title_bar_wait_field region-drag region null
title_bar_wait_field region-drag w 480
title_bar_wait_field region-drag h 300
"$UMBRIEL" settle >/dev/null
window=$(title_bar_window region-drag)
jq -e '.x <= 400 and .x + .w > 400 and .y <= 250 and .y + .h > 250' <<<"$window" >/dev/null
# Each later drag uses its current content center.
drag_to() {
  local x y
  read -r x y < <(title_bar_window region-drag | jq -r '"\(.x + .w / 2 | floor) \(.y + .h / 2 | floor)"')
  "$UMBRIEL_POINTER_CLIENT" 1280 720 move "$x" "$y" mod logo press 272 move "$1" "$2" release 272 mod none
}
drag_to 1279 0
title_bar_wait_field region-drag region top_right
title_bar_wait_box region-drag 628x348+644+8
drag_to 640 0
title_bar_wait_field region-drag region top
title_bar_wait_field region-drag maximized false
title_bar_wait_box region-drag 1264x348+8+8
drag_to 640 719
title_bar_wait_field region-drag region bottom
title_bar_wait_box region-drag 1264x348+8+364
drag_to 640 360
title_bar_wait_field region-drag region null
title_bar_wait_field region-drag w 480
title_bar_wait_field region-drag h 300
echo "floating drags preview and apply region zones, restoring normal size when detached"
