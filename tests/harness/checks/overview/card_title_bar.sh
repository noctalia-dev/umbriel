#!/usr/bin/env bash
# harness: outputs=1
# An overview card paints what its window resolved rather than reading it again per card: the mean of an "average" bar,
# and the contrast decision of a "window" bar whose edge is read as empty. Only a palette change may paint the palette.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/card-title-bar.log"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/card-title-bar.png"
readonly FIFO="$UMBRIEL_RUNTIME_DIR/card-title-bar.fifo"

fail() {
  echo "$1"
  exit 1
}

# $1 color_source, $2 extra [appearance.title_bar] lines
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

[appearance.shadow]
enabled = false

[appearance.blur]
enabled = false

[appearance.title_bar]
mode = "forced"
height = 28
color_source = "$1"
color_source_height = 1
button_background = false
button_glyph = "never"
title_shadow = false
${2:-}

[colors.title_bar]
background = "$3"
background_unfocused = "$3"
text = "$4"
text_unfocused = "$4"

[colors]
backdrop = "#000000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[overview]
zoom = 0.5

[[window_rule]]
match.title = "^card-fill\$"
default_floating = true
default_floating_size_px = { width = 480, height = 300 }
default_position = { x = 173, y = 109, anchor = "top_left" }
EOF
  "$UMBRIEL" msg config-reload > /dev/null
}

# The card is the window scaled by the overview zoom and placed above its workspace row: a 480x300 window at zoom 0.5
# puts its 28 px bar here, so counting this row alone separates the card from the live window below it.
readonly CARD_BAR="236x14+522+185"

count() { "$UMBRIEL_PIXEL_PROBE" "$SHOT" count "$1" "$CARD_BAR"; }

expect_at_least() {
  local pattern=$1 minimum=$2 what=$3 found
  found=$(count "$pattern")
  ((found >= minimum)) || fail "$what: only $found pixels match '$pattern'"
}

expect_none() {
  local pattern=$1 what=$2 found
  found=$(count "$pattern")
  ((found == 0)) || fail "$what: $found pixels still match '$pattern'"
}

# The mean of a light top edge, which the magenta palette cannot reach; the palette itself, which must not paint the bar
# while the mean is known; and the white a title reverses to on a dark bar.
readonly mean='r > 0.85 && r < 0.98 && g > 0.85 && g < 0.98 && b > 0.85 && b < 0.98'
readonly palette='r > 0.8 && g < 0.15 && b > 0.8'
readonly white='r > 0.98 && g > 0.98 && b > 0.98'

[[ -x $CLIENT ]] || fail "unmap client is not built"

# A reload can close the overview, so each card is captured from a close/open pair rather than assumed to be on screen.
shoot_card() {
  "$UMBRIEL" msg overview-close > /dev/null
  "$UMBRIEL" settle
  "$UMBRIEL" msg overview-open > /dev/null
  "$UMBRIEL" settle
  grim -o HEADLESS-1 "$SHOT"
}

# The rule has to be in place before the client maps: a reload does not re-place a mapped window.
configure average 'title_contrast = false' '#FF00FFFF' '#202020FF'
mkfifo "$FIFO"
FILL_COLOR=0xFF202020 TOP_BAND_COLOR=0xFFF0F0F0 TOP_BAND_HEIGHT=1 REPAINT_ON_STDIN=1 \
  "$CLIENT" card-title-bar 480 300 < "$FIFO" > "$LOG" 2>&1 &
exec 3> "$FIFO"
await_window card-title-bar || fail "the card client never mapped"

# The card carries the mean its window read; a card that dropped it falls back to the palette underneath.
shoot_card
expect_at_least "$mean" 800 "the card's averaged bar"
expect_none "$palette" "the palette painted the card's bar over the mean"

# The control: the same card under a palette it has to fall back to, which proves the assertion above is not vacuous.
configure fixed 'title_contrast = false' '#FF00FFFF' '#202020FF'
shoot_card
expect_at_least "$palette" 800 "the card's palette bar in fixed mode"

echo "an overview card paints the bar its window resolved, and the palette only when nothing was resolved"
