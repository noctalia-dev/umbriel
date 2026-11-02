#!/usr/bin/env bash
# harness: outputs=2
# A tiled drag into a stacking workspace has no insertion hint and becomes a policy float on release.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.title_bar]
mode = "forced"
[colors]
insert_hint = "#FF00FFFF"
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" msg output-focus-next >/dev/null
"$UMBRIEL" msg workspace-set-layout stacking >/dev/null
destination=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.focused) | .output')
"$UMBRIEL" msg output-focus-next >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" dragged 480 300 >"$UMBRIEL_RUNTIME_DIR/dragged.log" 2>&1 &
title_bar_wait_field dragged floating false
read -r x y < <(title_bar_window dragged | jq -r '"\(.x + .w / 2 | floor) \(.y + .h / 2 | floor)"')
dest_x=$("$UMBRIEL" outputs --json | jq -r --arg name "$destination" '.[] | select(.name == $name) | .position.x')
pointer_hold 2560 720 mod logo move "$x" "$y" press 272 \
  move $(((x + dest_x + 640) / 2)) 360 move $((dest_x + 640)) 360 -- release 272 mod none
"$UMBRIEL" settle >/dev/null
grim -o "$destination" "$UMBRIEL_RUNTIME_DIR/held.png"
hint=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/held.png" bbox 'r > 0.8 && g < 0.2 && b > 0.8')
[[ $hint == "0 0 0 0" ]] || {
  echo "unexpected tiled hint: $hint"
  exit 1
}
pointer_release
title_bar_wait_field dragged floating true
[[ $(title_bar_window dragged | jq -r .workspace) == "$destination:"* ]]
"$UMBRIEL" msg workspace-set-layout scrolling >/dev/null
title_bar_wait_field dragged floating false
# Title bars move policy floats without a modifier.
"$UMBRIEL" msg workspace-set-layout stacking >/dev/null
title_bar_wait_field dragged floating true
"$UMBRIEL" settle >/dev/null
read -r x y < <(title_bar_window dragged | jq -r '"\(.x + 80) \(.y - 14)"')
"$UMBRIEL_POINTER_CLIENT" 2560 720 move "$x" "$y" press 272 move $((x + 80)) $((y + 50)) release 272
"$UMBRIEL" settle >/dev/null
title_bar_wait_field dragged x "$x"
echo "floating destination drag and modifier-free title bar movement verified"
