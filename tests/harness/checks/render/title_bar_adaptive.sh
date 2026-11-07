#!/usr/bin/env bash
# With `[appearance.title_bar] color_source = "window"` the bar paints itself with the top strip of the window it
# belongs to instead of `[colors.title_bar] background`, and it follows the window when the application repaints that
# edge, without a config reload. Setting the key back to "fixed" restores the palette colour. The window paints its
# first five rows green over a red body, so a bar that sampled anything but that edge, or read more of it than it
# should, would show red.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/adaptive-bar.log"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/adaptive-bar.png"
# The floating window is 480x300 at 173,109, so its 28 px bar covers 173..653 by 81..109. The title text is drawn
# transparent and the buttons paint nothing, so this crop inside the bar reads the fill alone.
readonly BAR="120x10+233+87"

fail() {
  echo "$1"
  exit 1
}

# $1 color_source.
configure() {
  cat > "$UMBRIEL_CONFIG" <<EOF
[general]
xwayland = false
show_cheatsheet = false
autostart = []

[output.HEADLESS-1]
scale = 1.0

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.title_bar]
mode = "forced"
height = 28
color_source = "$1"
button_background = false
button_glyph = "never"

[appearance.shadow]
enabled = false

[appearance.blur]
enabled = false

[colors.title_bar]
background = "#FF00FFFF"
background_unfocused = "#FF00FFFF"
text = "#00000000"
text_unfocused = "#00000000"

[[window_rule]]
match.title = "^adaptive-bar\$"
default_floating = true
default_floating_size_px = { width = 480, height = 300 }
default_position = { x = 173, y = 109, anchor = "top_left" }
EOF
  "$UMBRIEL" msg config-reload > /dev/null
}

# Pixels in the bar crop matching a predicate.
count() { "$UMBRIEL_PIXEL_PROBE" "$FRAME" count "$1" "$BAR"; }

expect() {
  local predicate=$1 minimum=$2 what=$3 found
  found=$(count "$predicate")
  if ((found < minimum)); then
    fail "$what: only $found pixels match '$predicate'"
  fi
}

expect_none() {
  local predicate=$1 what=$2 found
  found=$(count "$predicate")
  if ((found != 0)); then
    fail "$what: $found pixels still match '$predicate'"
  fi
}

green='g > 0.9 && r < 0.1 && b < 0.1'
red='r > 0.9 && g < 0.1 && b < 0.1'
blue='b > 0.9 && r < 0.1 && g < 0.1'
magenta='r > 0.9 && b > 0.9 && g < 0.1'

[[ -x $CLIENT ]] || fail "unmap client is not built"

configure window

mkfifo "$UMBRIEL_RUNTIME_DIR/adaptive-bar.fifo"
FILL_COLOR=0xFFFF0000 TOP_BAND_COLOR=0xFF00FF00 TOP_BAND_HEIGHT=5 REPAINT_ON_STDIN=1 \
  "$CLIENT" adaptive-bar 480 300 < "$UMBRIEL_RUNTIME_DIR/adaptive-bar.fifo" > "$LOG" 2>&1 &
exec 3> "$UMBRIEL_RUNTIME_DIR/adaptive-bar.fifo"
send() { printf '%s' "$1" >&3; }

await_lines "$LOG" mapped 1
"$UMBRIEL" settle
grim "$FRAME"
# The bar takes the window's top edge, not its body, and not the palette's magenta.
expect "$green" 1000 "a window-coloured bar"
expect_none "$red" "the bar sampled the window body instead of its top edge"
expect_none "$magenta" "the palette colour stayed on a window-coloured bar"

# The application repaints the whole window; the bar follows with no reload in between.
send B
await_lines "$LOG" repainted 1
"$UMBRIEL" settle
grim "$FRAME"
expect "$blue" 1000 "a bar following a repaint"
expect_none "$green" "the bar kept the previous window colour"

# The palette colour is still the fixed choice, and the switch works live.
configure fixed
"$UMBRIEL" settle
grim "$FRAME"
expect "$magenta" 1000 "the palette colour after switching back to fixed"
expect_none "$red" "the window colour survived color_source = \"fixed\""
