#!/usr/bin/env bash
# harness: outputs=2
# Unplugging the output of a mirror's source with the overview open leaves the mirror with no source: its row loses the
# cards of the source's windows, and the overview shows what a freshly opened one does. Plugging the output back gives
# the mirror its source again, and the row its cards. The window is blue.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BLUE='b > 0.5 && r < 0.3 && g < 0.3'
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/client.log"

blue_count() {
  grim -o "$1" "$UMBRIEL_RUNTIME_DIR/$1.png"
  "$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/$1.png" count "$BLUE"
}

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
match.title = "^mirror-unplug$"
default_output = "HEADLESS-1"
default_workspace = "source"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 100, y = 100, anchor = "top_left" }
TOML
"$UMBRIEL" msg config-reload > /dev/null

FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" mirror-unplug 400 300 > "$CLIENT_LOG" 2>&1 &
await_window mirror-unplug
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
shown=$(blue_count HEADLESS-2)
if ((shown == 0)); then
  echo "the mirror row shows no card of the source's window"
  exit 1
fi

"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
live=$(blue_count HEADLESS-2)
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
fresh=$(blue_count HEADLESS-2)
if ((live != fresh)); then
  echo "after the unplug the open overview shows $live blue pixels, a freshly opened one $fresh"
  exit 1
fi
"$UMBRIEL" output-create HEADLESS-1 1280x720@60 > /dev/null
"$UMBRIEL" settle > /dev/null
replugged=$(blue_count HEADLESS-2)
if ((replugged != shown)); then
  echo "the source output is back: the mirror row shows $replugged blue pixels, not the $shown it showed before"
  exit 1
fi
echo "a mirror loses the cards of its source's windows with the source output, and gets them back with it"
