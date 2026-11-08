#!/usr/bin/env bash
# harness: outputs=2
# A mirror workspace owns no windows, so the overview row it gets on the receiving output shows its source's: a
# scaled card of each window, laid out through the mirror, made when the window maps, dropped when it leaves the
# source, and made again when it returns. Clicking such a card closes the overview onto the mirror without activating
# the source on its own output, and focuses the real window; middle-clicking it closes the real window, and every card
# of it leaves with it.
# The window is blue; the pixels and the bounding box of the blue are the card.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly BTN_MIDDLE=274
readonly LAYOUT_W=2240
readonly LAYOUT_H=720
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/client.log"
readonly BLUE='b > 0.5 && r < 0.3 && g < 0.3'

cat >> "$UMBRIEL_CONFIG" <<'EOF'

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
match.title = "^mirror-row$"
default_output = "HEADLESS-1"
default_workspace = "source"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r --arg output "$1" '[.[] | select(.output == $output and .active)] | .[0].name'
}

blue_count() {
  grim -o "$1" "$UMBRIEL_RUNTIME_DIR/$1.png"
  "$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/$1.png" count "$BLUE"
}

# The empty rows come first: nothing is blue until the window maps, and it maps with the overview open.
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
declare -A baseline
for output in HEADLESS-1 HEADLESS-2; do
  baseline[$output]=$(blue_count "$output")
done

FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" mirror-row 400 300 > "$CLIENT_LOG" 2>&1 &
await_window mirror-row
"$UMBRIEL" settle > /dev/null
window_id=$(window_by_title mirror-row | jq -r .id)

source_count=$(blue_count HEADLESS-1)
mirror_count=$(blue_count HEADLESS-2)
echo "overview window pixels: source row $source_count, mirror row $mirror_count (empty baselines ${baseline[HEADLESS-1]}," \
  "${baseline[HEADLESS-2]})"
if ((source_count <= baseline[HEADLESS-1])); then
  echo "the source row shows no window: the check does not see the client"
  exit 1
fi
# The source's overview row is its 1280x720 output at zoom 0.5, centered: 640x360 at 320,180. The receiver's is 480x360
# at 240,180, and its 960x720 output fits the source at 0.75 with 90 px above and below, so the copy is the source's
# card scaled by 0.75 and laid out through the mirror: 45 px down in the row.
read -r sx sy sw sh < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/HEADLESS-1.png" bbox "$BLUE")
read -r tx ty tw th < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/HEADLESS-2.png" bbox "$BLUE")
within() { (($1 >= $2 - 2 && $1 <= $2 + 2)); }
if ((tw == 0)) || ! within "$tw" $((sw * 3 / 4)) || ! within "$th" $((sh * 3 / 4)); then
  echo "the mirror row's card should be the source's scaled by 0.75: source ${sw}x${sh}, mirror ${tw}x${th}"
  exit 1
fi
if ! within "$((tx - 240))" $(((sx - 320) * 3 / 4)) || ! within "$((ty - 180))" $(((sy - 180) * 3 / 4 + 45)); then
  echo "the mirror row's card is misplaced: source at $sx,$sy, mirror at $tx,$ty"
  exit 1
fi

# The window leaves for a workspace the receiver does not show, and its copy goes with it; back home, the copy returns.
"$UMBRIEL" msg window-move-to-workspace-silent 2 --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
away_count=$(blue_count HEADLESS-2)
if ((away_count > baseline[HEADLESS-2])); then
  echo "the window left the mirror's source but the mirror row still shows $away_count blue pixels"
  exit 1
fi
"$UMBRIEL" msg window-focus "$window_id" > /dev/null
"$UMBRIEL" msg window-move-to-workspace-silent 1 --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
back_count=$(blue_count HEADLESS-2)
if ((back_count != mirror_count)); then
  echo "the window came back to the mirror's source: the mirror row shows $back_count blue pixels, not $mirror_count"
  exit 1
fi

# Out of the way of the source: its own output shows another workspace, and the receiver shows its own, so the
# mirror's card is one a row away from the one on screen and a click on it has to switch the receiver to the mirror.
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" msg workspace-switch own --output HEADLESS-2 > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/receiver.png"
read -r bx by bw bh < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/receiver.png" bbox "$BLUE")
if ((bw == 0)); then
  echo "the receiver's overview shows no card of the mirror's window beside its own row"
  exit 1
fi
if [[ $(window_by_title mirror-row | jq -r .focused) == true ]]; then
  echo "the window is focused before the click: the click cannot show that it focuses it"
  exit 1
fi
entered=$(events "$CLIENT_LOG" keyboard-enter)
card_x=$((1280 + bx + bw / 2))
card_y=$((by + bh / 2))
pointer() { "$UMBRIEL_POINTER_CLIENT" "$LAYOUT_W" "$LAYOUT_H" "$@"; }
pointer move "$card_x" "$card_y" click "$BTN_LEFT"
"$UMBRIEL" settle > /dev/null

if [[ $(active_workspace HEADLESS-2) != source ]]; then
  echo "clicking the mirror's card left HEADLESS-2 on '$(active_workspace HEADLESS-2)', expected the mirror 'source'"
  exit 1
fi
if [[ $(active_workspace HEADLESS-1) != away ]]; then
  echo "clicking the mirror's card changed HEADLESS-1 to '$(active_workspace HEADLESS-1)'"
  exit 1
fi
if [[ $(window_by_title mirror-row | jq -r .focused) != true ]]; then
  echo "clicking the mirror's card did not focus the real window: $(window_by_title mirror-row)"
  exit 1
fi
await_lines "$CLIENT_LOG" keyboard-enter $((entered + 1))

# The source shows on both outputs, so the window is drawn on both overviews.
"$UMBRIEL" msg workspace-switch source --output HEADLESS-1 > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
for output in HEADLESS-1 HEADLESS-2; do
  shown=$(blue_count "$output")
  if ((shown <= baseline[$output])); then
    echo "$output shows no card of the window with the source on both outputs ($shown blue pixels)"
    exit 1
  fi
done

# Both cards leave with the window: a middle click closes the real one, and no blue is left on either output.
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/receiver.png"
read -r bx by bw bh < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/receiver.png" bbox "$BLUE")
if ((bw == 0)); then
  echo "the receiver's overview shows no card to close"
  exit 1
fi
pointer move "$((1280 + bx + bw / 2))" "$((by + bh / 2))" click "$BTN_MIDDLE"
await_lines "$CLIENT_LOG" unmapped 1
"$UMBRIEL" settle > /dev/null
for output in HEADLESS-1 HEADLESS-2; do
  left=$(blue_count "$output")
  if ((left > baseline[$output])); then
    echo "the window closed but $output still shows $left blue pixels (empty baseline ${baseline[$output]})"
    exit 1
  fi
done
echo "the mirror row shows its source's window scaled and placed through the mirror, selects the window, and loses it" \
  "when it closes"
