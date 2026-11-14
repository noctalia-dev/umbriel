#!/usr/bin/env bash
# harness: outputs=2
# A mirror presents the whole source window: the server-side title bar and the border scale with it, not only the
# client's surface.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
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
border_width = 6
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "forced"
height = 40
[colors.title_bar]
background = "#FF0000FF"
background_unfocused = "#FF0000FF"
close = "#FFFF00FF"
[colors.border]
focused = "#FF00FFFF"
unfocused = "#FF00FFFF"
[[window_rule]]
match.title = "^mirror-decorated$"
default_output = "HEADLESS-1"
default_workspace = "source"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 100, y = 100, anchor = "top_left" }
TOML
"$UMBRIEL" msg config-reload > /dev/null
env XDG_DECORATION=server "$UMBRIEL_FRACTIONAL_CLIENT" mirror-decorated 400 300 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
await_window mirror-decorated
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/source.png"
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/copy.png"
# The receiver is 0.75 of the source in each axis, so a decoration covers about 0.75^2 of the pixels in the copy (a
# little less for the border, whose 6 pixels round to 5).
assert_scaled() {
  local name=$1 predicate=$2 source_count copy_count
  source_count=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/source.png" count "$predicate")
  copy_count=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/copy.png" count "$predicate")
  echo "$name pixels: source $source_count, copy $copy_count"
  if ((source_count == 0 || copy_count * 100 < source_count * 45 || copy_count * 100 > source_count * 68)); then
    echo "the mirror shows $copy_count $name pixels, expected about $((source_count * 9 / 16))"
    exit 1
  fi
}
red='r > 0.8 && g < 0.2 && b < 0.2'
assert_scaled title-bar "$red"
assert_scaled border 'r > 0.8 && b > 0.8 && g < 0.2'
# The title bar sits where the mirror maps it: scaled by 0.75 and below the 90-pixel band the fit leaves on top.
read -r sx sy sw _ < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/source.png" bbox "$red")
read -r tx ty tw _ < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/copy.png" bbox "$red")
if ((tx < sx * 3 / 4 - 2 || tx > sx * 3 / 4 + 2 || ty < sy * 3 / 4 + 88 || ty > sy * 3 / 4 + 92
  || tw < sw * 3 / 4 - 2 || tw > sw * 3 / 4 + 2)); then
  echo "the mirrored title bar is at $tx,$ty ${tw} wide, expected $((sx * 3 / 4)),$((sy * 3 / 4 + 90)) $((sw * 3 / 4)) wide"
  exit 1
fi
# A move that neither commits nor re-presents, with the source shown only through the mirror, still moves the copy.
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" msg window-move-by 200 0 --window "$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "mirror-decorated") | .id')" > /dev/null
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/moved.png"
read -r mx _ _ _ < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/moved.png" bbox "$red")
if ((mx < tx + 148 || mx > tx + 152)); then
  echo "the mirrored title bar stayed at x=$mx after the window moved 200 pixels, expected about $((tx + 150))"
  exit 1
fi
# The copied title bar takes the pointer for the real window: dragging it 75 receiving pixels moves the window 100. The
# grab starts on a multiple of 3 receiving pixels so 0.75 maps it to a whole source pixel.
window_x() { "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "mirror-decorated") | .x'; }
before=$(window_x)
grab_x=$((1280 + (mx + tw / 2) / 3 * 3))
grab_y=$((ty + 15))
"$UMBRIEL_POINTER_CLIENT" 2240 720 move "$grab_x" "$grab_y" press 272 move $((grab_x + 75)) "$grab_y" release 272
"$UMBRIEL" settle > /dev/null
after=$(window_x)
if ((after != before + 100)); then
  echo "dragging the mirrored title bar moved the window from x=$before to x=$after, expected $((before + 100))"
  exit 1
fi
"$UMBRIEL" workspaces --json | jq -e 'any(.[]; .output == "HEADLESS-1" and .name == "away" and .active)' > /dev/null
# Its close button closes the real window.
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/buttons.png"
read -r cx cy cw ch < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/buttons.png" bbox 'r > 0.8 && g > 0.8 && b < 0.2')
if ((cw == 0)); then
  echo "the mirrored title bar shows no close button"
  exit 1
fi
"$UMBRIEL_POINTER_CLIENT" 2240 720 move $((1280 + cx + cw / 2)) $((cy + ch / 2)) click 272
for _ in $(seq 80); do
  [[ -z $("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "mirror-decorated") | .id') ]] && break
  sleep 0.025
done
if [[ -n $("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "mirror-decorated") | .id') ]]; then
  echo "clicking the mirrored close button left the window open"
  exit 1
fi
echo 'The mirror scales the source window frame in place and follows a move; its title bar moves and closes the window.'
