#!/usr/bin/env bash
# harness: outputs=2
# A card held on the mirror's row belongs to a window that can leave the mirror's source under the pointer: the window
# moves to another workspace while the card is held. The card goes with the mirror's cards of it, the drag ends without
# a drop, and the release lands nowhere. While the card is held, the window's other cards do not stay behind.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly LAYOUT_W=2240
readonly LAYOUT_H=720
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/client.log"
readonly BLUE='b > 0.5 && r < 0.3 && g < 0.3'
readonly LIFTED='b > 0.1 && r < 0.1 && g < 0.1'

cat >> "$UMBRIEL_CONFIG" <<'TOML'

[animation]
enabled = false

[output.HEADLESS-1]
mode = "1280x720@60"
position = [0, 0]
workspaces = ["source", "away"]

[output.HEADLESS-2]
mode = "960x720@60"
position = [1280, 0]
workspaces = ["own", "source"]

[[workspace]]
output = "HEADLESS-2"
name = "source"
mirror = { output = "HEADLESS-1", name = "source" }

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[colors]
backdrop = "#000000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[overview]
zoom = 0.5
background_blur = false
shortcuts = false

[[window_rule]]
match.title = "^mirror-held$"
default_output = "HEADLESS-1"
default_workspace = "source"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 100, y = 100, anchor = "top_left" }
TOML
"$UMBRIEL" msg config-reload > /dev/null

FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" mirror-held 400 300 > "$CLIENT_LOG" 2>&1 &
await_window mirror-held
"$UMBRIEL" settle > /dev/null
window_id=$(window_by_title mirror-held | jq -r .id)
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/receiver.png"
read -r bx by bw bh < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/receiver.png" bbox "$BLUE")
if ((bw == 0)); then
  echo "the mirror row shows no card of the window to hold"
  exit 1
fi
card_x=$((1280 + bx + bw / 2))
card_y=$((by + bh / 2))
grim -o HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/source.png"
if (($("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/source.png" count "$BLUE") == 0)); then
  echo "the source's own row shows no card of the window before it is held"
  exit 1
fi
pointer_hold "$LAYOUT_W" "$LAYOUT_H" move "$card_x" "$card_y" press "$BTN_LEFT" move "$((card_x + 30))" "$card_y"
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/held.png"
read -r hx _ _ _ < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/held.png" bbox "$LIFTED")
if ((hx < bx + 28 || hx > bx + 32)); then
  echo "the held card is not at the pointer: it began at x=$bx and sits at x=$hx, expected a 30 px move"
  pointer_release
  exit 1
fi
# The window is lifted: its card on the source's own row does not stay behind.
grim -o HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/source.png"
if (($("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/source.png" count "$BLUE") != 0)); then
  echo "the source's own row still shows the card of the window held on the mirror's row"
  pointer_release
  exit 1
fi
"$UMBRIEL" msg window-focus "$window_id" > /dev/null
"$UMBRIEL" msg window-move-to-workspace-silent 2 --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
pointer_release
"$UMBRIEL" settle > /dev/null
moved=$(window_by_title mirror-held)
if [[ $(jq -r .workspace <<< "$moved") != HEADLESS-1:2 ]]; then
  echo "the release moved the window to '$(jq -r .workspace <<< "$moved")', expected it to stay on HEADLESS-1:2"
  exit 1
fi
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/after.png"
if (($("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/after.png" count "$BLUE") != 0)); then
  echo "the mirror row still shows the window that left its source"
  exit 1
fi
echo "a window that leaves the mirror's source while its mirror card is held ends the drag without a drop"
