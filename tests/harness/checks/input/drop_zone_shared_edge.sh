#!/usr/bin/env bash
# harness: outputs=2
# Crossing a shared edge is immediate; region placement there requires staying on the same side for the dwell.
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
match.title = "^shared-drag$"
default_floating = true
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" shared-drag 480 300 >"$UMBRIEL_RUNTIME_DIR/shared-drag.log" 2>&1 &
title_bar_wait_field shared-drag floating true
read -r x y output < <(title_bar_window shared-drag | jq -r '"\(.x + .w / 2 | floor) \(.y + .h / 2 | floor) \(.output)"')
left=$("$UMBRIEL" outputs --json | jq -r --arg name "$output" '.[] | select(.name == $name) | .position.x')
if ((left == 0)); then
  edge=1279
  region=right
else
  edge=1280
  region=left
fi
# Reach the seam and release without any intervening round trips.
"$UMBRIEL_POINTER_CLIENT" 2560 720 move "$x" "$y" mod logo press 272 move-release "$edge" 360 272 mod none
title_bar_wait_field shared-drag region null
# Stay on this side of the same shared edge until the preview becomes visible.
read -r x y < <(title_bar_window shared-drag | jq -r '"\(.x + .w / 2 | floor) \(.y + .h / 2 | floor)"')
pointer_hold 2560 720 move "$x" "$y" mod logo press 272 move "$edge" 360 -- release 272 mod none
for _ in $(seq 60); do
  grim -o "$output" "$UMBRIEL_RUNTIME_DIR/shared-preview.png"
  bounds=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/shared-preview.png" bbox 'r < 0.1 && g > 0.9 && b > 0.9')
  [[ $bounds != '0 0 0 0' ]] && break
  sleep 0.025
done
if [[ $region == right ]]; then expected='644 8 628 704'; else expected='8 8 628 704'; fi
[[ $bounds == "$expected" ]] || {
  echo "shared edge preview missing or incorrect: $bounds"
  exit 1
}
pointer_release
title_bar_wait_field shared-drag region "$region"
echo "shared output edges region after a dwell and allow immediate crossing"
