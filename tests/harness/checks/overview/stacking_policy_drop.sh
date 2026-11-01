#!/usr/bin/env bash
# harness: outputs=2
# Overview moves obey destination layout policy in both directions.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "off"
[overview]
zoom = 0.5
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" msg output-focus-next >/dev/null
"$UMBRIEL" msg workspace-set-layout stacking >/dev/null
"$UMBRIEL" msg output-focus-next >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" card 480 300 >"$UMBRIEL_RUNTIME_DIR/card.log" 2>&1 &
title_bar_wait_field card floating false
"$UMBRIEL" settle >/dev/null
origin=$(title_bar_window card | jq -r '.workspace | split(":")[0]')
destination=$("$UMBRIEL" outputs --json | jq -r --arg origin "$origin" '.[] | select(.name != $origin) | .name')
"$UMBRIEL" msg overview-open >/dev/null
"$UMBRIEL" settle >/dev/null
# Each 1280x720 output has a 640x360 preview centered at +320+180.
drag_to() {
  local dest=$1 output left dest_left x y
  output=$(title_bar_window card | jq -r '.workspace | split(":")[0]')
  left=$("$UMBRIEL" outputs --json | jq -r --arg name "$output" '.[] | select(.name == $name) | .position.x')
  dest_left=$("$UMBRIEL" outputs --json | jq -r --arg name "$dest" '.[] | select(.name == $name) | .position.x')
  read -r x y < <(title_bar_window card | jq -r --argjson left "$left" \
    '"\($left + 320 + (.x - $left + .w / 2) / 2 | floor) \(180 + (.y + .h / 2) / 2 | floor)"')
  "$UMBRIEL_POINTER_CLIENT" 2560 720 move "$x" "$y" press 272 \
    move $(((x + dest_left + 640) / 2)) 360 move $((dest_left + 640)) 360 pause 100 release 272
  "$UMBRIEL" settle >/dev/null
  title_bar_wait_field card workspace "$dest:1"
}
drag_to "$destination"
title_bar_wait_field card floating true
drag_to "$origin"
title_bar_wait_field card floating false
"$UMBRIEL" msg overview-close >/dev/null
"$UMBRIEL" settle >/dev/null
echo "overview tiled and floating policy transfers verified"
