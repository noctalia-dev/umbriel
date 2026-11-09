#!/usr/bin/env bash
# harness: outputs=1
# A title whose colour would vanish into the bar reverses to black or white, the dark edge behind it is on in every
# color_source and one key turns it off, and `color_source = "window"` only reverses when it is allowed to sample.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/title-bar-contrast.log"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/title-bar-contrast.png"
# The floating window is 480x300 at 173,109, so its 28 px bar covers 173..653 by 81..109, inset by a pixel here.
readonly BAR="474x26+175+82"

fail() {
  echo "$1"
  exit 1
}

# $1 color_source, $2 bar colour, $3 title colour, $4 extra [appearance.title_bar] lines, $5 extra [colors.title_bar],
# $6 button glyph visibility.
configure() {
  local source=$1 background=$2 text=$3 title_bar=${4:-} colors=${5:-} glyph=${6:-never}
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
color_source = "$source"
button_position = "right"
button_glyph = "$glyph"
button_background = false
$title_bar

[colors.title_bar]
background = "$background"
background_unfocused = "$background"
text = "$text"
text_unfocused = "$text"
$colors

[[window_rule]]
match.title = "^contrast-bar\$"
default_floating = true
default_floating_size_px = { width = 480, height = 300 }
default_position = { x = 173, y = 109, anchor = "top_left" }
EOF
  "$UMBRIEL" msg config-reload > /dev/null
}

# The same bar with visible symbols, drawn on the bar itself (button_background is off), so a symbol's own colour can be
# probed against the colour it sits on.
configure_buttons() {
  configure fixed "$1" "$2" "$3" "close_glyph = \"$2\"
maximize_glyph = \"$2\"
minimize_glyph = \"$2\"" always
}

count() { "$UMBRIEL_PIXEL_PROBE" "$FRAME" count "$1" "$BAR"; }

expect_at_least() {
  local predicate=$1 minimum=$2 what=$3 found
  found=$(count "$predicate")
  ((found >= minimum)) || fail "$what: only $found pixels match '$predicate'"
}

expect_none() {
  local predicate=$1 what=$2 found
  found=$(count "$predicate")
  ((found == 0)) || fail "$what: $found pixels still match '$predicate'"
}

# A reversal paints pure white, which the palette's light grey (#E8E8E8) cannot reach. Counting it is how a flip is told
# apart from a configured colour that merely reads well on its own.
readonly white='r > 0.98 && g > 0.98 && b > 0.98'
readonly light='r > 0.85 && r < 0.96 && g > 0.85 && g < 0.96 && b > 0.85 && b < 0.96'
readonly dark='r < 0.5 && g < 0.5 && b < 0.5'

[[ -x $CLIENT ]] || fail "unmap client is not built"

# The window rule places the bar, so it has to be in place before the client maps: a reload does not re-place a mapped
# window. The top edge is light, so the dark edge under the title has something to show against.
configure fixed '#101010FF' '#181818FF'
mkfifo "$UMBRIEL_RUNTIME_DIR/title-bar-contrast.fifo"
FILL_COLOR=0xFFFF0000 TOP_BAND_COLOR=0xFFF0F0F0 TOP_BAND_HEIGHT=1 \
  "$CLIENT" contrast-bar 480 300 < "$UMBRIEL_RUNTIME_DIR/title-bar-contrast.fifo" > "$LOG" 2>&1 &
exec 3> "$UMBRIEL_RUNTIME_DIR/title-bar-contrast.fifo"
shot() {
  "$UMBRIEL" settle
  grim -o HEADLESS-1 "$FRAME"
}
await_lines "$LOG" mapped 1

# A dark title on a dark bar reverses to white.
configure fixed '#101010FF' '#181818FF'
shot
expect_at_least "$white" 10 "a title that reverses on a dark bar"

# The same pair with the reversal off keeps the configured colour, so nothing bright is drawn.
configure fixed '#101010FF' '#181818FF' 'title_contrast = false'
shot
expect_none "$white" "title_contrast = false still reversed the title"

# A pair that already reads is left alone: the configured grey is drawn rather than black or white.
configure fixed '#101010FF' '#E8E8E8FF'
shot
expect_at_least "$light" 10 "a title whose colour already reads"
expect_none "$white" "a readable title was reversed anyway"

# The dark edge follows its own key, not the colour source. A white title on a white bar is invisible, so every dark
# pixel in the bar is the edge itself.
configure fixed '#FFFFFFFF' '#FFFFFFFF' 'title_contrast = false'
shot
expect_at_least "$dark" 10 "the title's dark edge on a fixed bar"
configure fixed '#FFFFFFFF' '#FFFFFFFF' 'title_contrast = false
title_shadow = false'
shot
expect_none "$dark" "title_shadow = false still shaded the title"

# The same edge on an adaptive bar, and the same key turns it off there.
configure window '#FFFFFFFF' '#FFFFFFFF' 'title_contrast = false'
shot
expect_at_least "$dark" 10 "the title's dark edge on a window-coloured bar"
configure window '#FFFFFFFF' '#FFFFFFFF' 'title_contrast = false
title_shadow = false'
shot
expect_none "$dark" "title_shadow = false still shaded a window-coloured title"

# A window-coloured bar has no colour to judge the title against, so it does not reverse until it is allowed to sample.
# The edge is light and the title is light grey, which only a reversal makes readable.
configure window '#FFFFFFFF' '#E8E8E8FF' 'title_shadow = false'
shot
expect_none "$dark" "a window-coloured title reversed without the sample"
configure window '#FFFFFFFF' '#E8E8E8FF' 'title_shadow = false
title_contrast_sample = true'
shot
expect_at_least "$dark" 10 "a title reversing on a sampled window-coloured bar"

# The reversal reaches the symbols drawn straight on the bar, because they sit on that colour too. The edge is off so
# that a dark pixel can only be the symbol itself.
configure_buttons '#FFFFFFFF' '#B0B0B0FF' 'title_shadow = false'
shot
expect_at_least "$dark" 5 "a symbol reversing against the bar it sits on"
configure_buttons '#FFFFFFFF' '#B0B0B0FF' 'title_shadow = false
title_contrast = false'
shot
expect_none "$dark" "title_contrast = false still reversed a symbol"

echo "the edge follows title_shadow in fixed and window modes, the title reverses only when its colour would vanish, and the symbols follow the same rule"
