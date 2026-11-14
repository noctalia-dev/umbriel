#!/usr/bin/env bash
# harness: outputs=2
# A window of the source output's other workspace, dragged onto the mirror's row on the receiving output, lands in the
# workspace the mirror shows and never in the mirror itself.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly LAYOUT_W=2240
readonly LAYOUT_H=720
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/client.log"
readonly RED='r > 0.5 && b < 0.3 && g < 0.3'

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
match.title = "^mirror-away$"
default_output = "HEADLESS-1"
default_workspace = "away"
default_floating = false
TOML
"$UMBRIEL" msg config-reload > /dev/null

FILL_COLOR=0xFFFF0000 "$UMBRIEL_UNMAP_CLIENT" mirror-away 400 300 > "$CLIENT_LOG" 2>&1 &
await_window mirror-away
"$UMBRIEL" settle > /dev/null
if [[ $(window_by_title mirror-away | jq -r .workspace) != HEADLESS-1:2 ]]; then
  echo "the dragged window starts on '$(window_by_title mirror-away | jq -r .workspace)', not 'away' (HEADLESS-1:2)"
  exit 1
fi
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" msg workspace-switch own --output HEADLESS-2 > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/source.png"
read -r rx ry rw rh < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/source.png" bbox "$RED")
if ((rw == 0)); then
  echo "the source output's overview shows no card of the window to drag"
  exit 1
fi
# The mirror's row is the one below the receiver's own, and its top 144 rows show at the bottom of the output.
"$UMBRIEL_POINTER_CLIENT" "$LAYOUT_W" "$LAYOUT_H" move "$((rx + rw / 2))" "$((ry + rh / 2))" press "$BTN_LEFT" \
  move "$((rx + rw / 2 + 30))" "$((ry + rh / 2))" move 1760 650 move 1764 654 release "$BTN_LEFT"
"$UMBRIEL" settle > /dev/null
dropped=$(window_by_title mirror-away)
if [[ $(jq -r .workspace <<< "$dropped") != HEADLESS-1:1 || $(jq -r .output <<< "$dropped") != HEADLESS-1 ]]; then
  echo "a drop on the mirror row left the window on '$(jq -r .workspace <<< "$dropped")' of" \
    "$(jq -r .output <<< "$dropped"), expected 'source' (HEADLESS-1:1) of HEADLESS-1"
  exit 1
fi
echo "a window dropped on the mirror row lands in the workspace the mirror shows"
