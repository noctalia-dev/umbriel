#!/usr/bin/env bash
# harness: outputs=2
# A [[scratchpad]] with layout.mode tiles its windows with that layout as soon as its layout changes, layout actions on
# a focused scratchpad window act on the scratchpad's layout, a scrolling strip stays on the scratchpad's output, and
# dropping layout.mode on reload floats its windows again. Animations stay on so the layout's motion must settle.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly A=pad-tile-a
readonly B=pad-tile-b
readonly C=pad-tile-c

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[scratchpad]]
name = "tiles"

[[window_rule]]
match.title = "^pad-tile-"
default_scratchpad = "tiles"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn() {
  FILL_COLOR=$2 RESIZE_FILL_COLOR=$2 "$UMBRIEL_UNMAP_CLIENT" "$1" 400 300 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  await_window "$1"
}
spawn "$A" 0xFFFF0000
spawn "$B" 0xFF00FF00
spawn "$C" 0xFF0000FF
"$UMBRIEL" msg scratchpad-toggle tiles > /dev/null
"$UMBRIEL" settle

windows() { "$UMBRIEL" windows --json; }
field() { window_by_title "$1" | jq -r ".$2"; }

# The floating pad's windows switch to tiles that cover distinct parts of the output, each redrawn at its tile size.
"$UMBRIEL" msg window-focus "$(field "$A" id)" > /dev/null
"$UMBRIEL" msg workspace-set-layout dwindle > /dev/null
"$UMBRIEL" settle
if ! windows | jq -e '
  [.[] | select(.title | startswith("pad-tile-"))] as $tiles
  | ($tiles | length) == 3
  and all($tiles[]; .scratchpad == "tiles" and .workspace == "" and .floating == false and .visible)
  and ([$tiles[] | .w * .h] | add) > 1280 * 720 * 0.8
  and all($tiles[] as $p | $tiles[] as $q | $p.title >= $q.title
    or $p.x + $p.w <= $q.x or $q.x + $q.w <= $p.x or $p.y + $p.h <= $q.y or $q.y + $q.h <= $p.y)
' > /dev/null; then
  echo "dwindle scratchpad did not tile its windows: $(windows)"
  exit 1
fi

"$UMBRIEL" msg window-focus "$(field "$A" id)" > /dev/null
before=$(windows | jq -c '[.[] | select(.title | startswith("pad-tile-")) | {title, x, y}] | sort_by(.title)')
"$UMBRIEL" msg window-swap-next > /dev/null
"$UMBRIEL" settle
after=$(windows | jq -c '[.[] | select(.title | startswith("pad-tile-")) | {title, x, y}] | sort_by(.title)')
if [[ $before == "$after" || $(field "$A" active) != true ]]; then
  echo "window-swap-next did not swap the focused scratchpad tile: $before -> $after"
  exit 1
fi

# A full-width column pushes the rest of the strip past the output edge; none of it may draw on the other output.
"$UMBRIEL" msg workspace-set-layout scrolling > /dev/null
"$UMBRIEL" msg window-toggle-maximize > /dev/null
"$UMBRIEL" settle
if (($(field "$A" x) < 1280)); then
  neighbour=1280x720+1280+0
else
  neighbour=1280x720+0+0
fi
grim "$UMBRIEL_RUNTIME_DIR/strip.png"
read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/strip.png" count \
  'r > 0.7 && g < 0.2 && b < 0.2' 'r < 0.2 && g > 0.7 && b < 0.2' 'r < 0.2 && g < 0.2 && b > 0.7' "$neighbour")
if ((red + green + blue != 0)); then
  echo "the scrolling scratchpad drew on the neighbouring output: $red $green $blue pixels: $(windows)"
  exit 1
fi

"$UMBRIEL" config set scratchpad '[{ name = "tiles", layout.mode = "master" }]' > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" config set scratchpad '[{ name = "tiles" }]' > /dev/null
"$UMBRIEL" settle
if ! windows | jq -e '[.[] | select(.title | startswith("pad-tile-"))]
  | length == 3 and all(.scratchpad == "tiles" and .floating == true)' > /dev/null; then
  echo "a reload without layout.mode did not float the scratchpad's windows: $(windows)"
  exit 1
fi

echo "a scratchpad tiled its windows on a layout change, kept layout actions and its strip to itself, and floated again"
