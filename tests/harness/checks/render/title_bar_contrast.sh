#!/usr/bin/env bash
# harness: outputs=1
# A title whose colour would vanish into the bar reverses to black or white, the dark edge behind it and around its
# symbols is on in every color_source and one key turns it off, and `color_source = "window"` only reverses — and only
# reads the edge back — when it is allowed to sample.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/title-bar-contrast.log"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/title-bar-contrast.png"
# The floating window is 480x300 at 173,109, so its 28 px bar covers 173..653 by 81..109, inset by a pixel here. The
# button row sits at the bar's right edge, where a short title never reaches.
readonly BAR="474x26+175+82"
readonly BUTTONS="90x26+555+82"

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

# The same bar with visible symbols drawn on the bar itself (button_background is off) and the title in the bar's own
# colour, so only the symbols and their edges paint anything.
configure_buttons() {
  configure fixed "$1" "$1" "$3" "close_glyph = \"$2\"
maximize_glyph = \"$2\"
minimize_glyph = \"$2\"" always
}

count() { "$UMBRIEL_PIXEL_PROBE" "$FRAME" count "$1" "${2:-$BAR}"; }

expect_at_least() {
  local predicate=$1 minimum=$2 what=$3 region=${4:-$BAR} found
  found=$(count "$predicate" "$region")
  ((found >= minimum)) || fail "$what: only $found pixels match '$predicate'"
}

expect_none() {
  local predicate=$1 what=$2 region=${3:-$BAR} found
  found=$(count "$predicate" "$region")
  ((found == 0)) || fail "$what: $found pixels still match '$predicate'"
}

# A reversal paints pure white, which the palette's light grey (#E8E8E8) cannot reach. Counting it is how a flip is told
# apart from a configured colour that merely reads well on its own.
readonly white='r > 0.98 && g > 0.98 && b > 0.98'
readonly light='r > 0.85 && r < 0.96 && g > 0.85 && g < 0.96 && b > 0.85 && b < 0.96'
readonly dark='r < 0.5 && g < 0.5 && b < 0.5'
# A reversal onto a light bar paints pure black, which no colour configured here reaches.
readonly black='r < 0.02 && g < 0.02 && b < 0.02'
# Anything that is not the bar's own white: an edge thinner than a glyph stroke only ever lands between the two.
readonly notwhite='r < 0.98 || g < 0.98 || b < 0.98'

[[ -x $CLIENT ]] || fail "unmap client is not built"

# The window rule places the bar, so it has to be in place before the client maps: a reload does not re-place a mapped
# window. The top edge is light, so the dark edge under the title has something to show against.
configure fixed '#101010FF' '#181818FF'
mkfifo "$UMBRIEL_RUNTIME_DIR/title-bar-contrast.fifo"
FILL_COLOR=0xFFFF0000 TOP_BAND_COLOR=0xFFF0F0F0 TOP_BAND_HEIGHT=1 REPAINT_ON_STDIN=1 \
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

# A window-coloured bar has no colour to judge the title against, and the palette is not what shows, so it does not
# reverse until it is allowed to sample. The palette is dark and the edge light here, so only a sample of the edge can
# flip the title — a decision taken on the palette would leave it light grey.
readbacks() {
  local found
  found=$(rg -c 'averaged a [0-9]+x[0-9]+ strip read back' "$UMBRIEL_RUNTIME_DIR/compositor.log" || true)
  echo "${found:-0}"
}
configure window '#101010FF' '#E8E8E8FF' 'title_shadow = false'
shot
expect_none "$dark" "a window-coloured title reversed without the sample"
(( $(readbacks) == 0 )) || fail "title_contrast_sample = false still read the edge back"
configure window '#101010FF' '#E8E8E8FF' 'title_shadow = false
title_contrast_sample = true'
shot
expect_at_least "$dark" 10 "a title reversing on a sampled window-coloured bar"
(( $(readbacks) > 0 )) || fail "title_contrast_sample = true did not read the edge back"

# Turning the sample off again drops the colour it gave: the bar is back to being unknown, so the title stands as
# configured rather than reversing against an edge nobody reads any more.
configure window '#101010FF' '#E8E8E8FF' 'title_shadow = false
title_contrast_sample = false'
shot
expect_none "$dark" "a kept sample kept reversing the title after the key was turned off"

# "average" knows the bar's colour without a key, so the same reversal happens there against the mean.
configure average '#101010FF' '#E8E8E8FF' 'title_shadow = false'
shot
expect_at_least "$dark" 10 "a title reversing against an averaged bar"
configure average '#101010FF' '#E8E8E8FF' 'title_shadow = false
title_contrast = false'
shot
expect_none "$dark" "title_contrast = false still reversed an averaged title"

# A symbol can flip while the title does not: the fill moved, the title's own decision did not. Nothing but the commit
# hook can notice that, since the symbols are painted by a repaint rather than by the mirror. The title is chosen to read
# on both edges; the symbols reverse only on the green one, which a repaint reaches without touching the title colour.
configure window '#101010FF' '#505050FF' 'title_shadow = false
title_contrast_sample = true' 'close_glyph = "#737373FF"
maximize_glyph = "#737373FF"
minimize_glyph = "#737373FF"' always
shot
expect_none "$black" "a symbol reversed on an edge it reads against"
printf 'G' >&3
await_lines "$LOG" repainted 1
shot
expect_at_least "$black" 1 "a symbol left behind after the edge changed under it"

# An edge with nothing in it leaves the palette showing, so that is what the title is judged against — and it is read
# live, not remembered from the moment of the sample: the same window under a dark palette reverses, under a light one
# it stays as configured.
configure window '#E8E8E8FF' '#202020FF' 'title_shadow = false
title_contrast_sample = true'
printf 't' >&3
await_lines "$LOG" repainted 2
shot
expect_none "$white" "a title judged against a light palette reversed anyway"
configure window '#000000FF' '#202020FF' 'title_shadow = false
title_contrast_sample = true'
shot
expect_at_least "$white" 10 "a title still judged against the palette of the previous sample"

# The reversal reaches the symbols drawn straight on the bar, because they sit on that colour too. Only the button row is
# counted, so the title's own reversal cannot stand in for a symbol's.
configure_buttons '#FFFFFFFF' '#B0B0B0FF' 'title_shadow = false'
shot
expect_at_least "$dark" 5 "a symbol reversing against the bar it sits on" "$BUTTONS"
configure_buttons '#FFFFFFFF' '#B0B0B0FF' 'title_shadow = false
title_contrast = false'
shot
expect_none "$dark" "title_contrast = false still reversed a symbol" "$BUTTONS"

# The edge follows title_shadow around the symbols as well: a white symbol on a white bar paints nothing, so every pixel
# that is not white in the button row is the edge itself.
configure_buttons '#FFFFFFFF' '#FFFFFFFF' 'title_contrast = false'
shot
expect_at_least "$notwhite" 5 "the symbols' dark edge" "$BUTTONS"
configure_buttons '#FFFFFFFF' '#FFFFFFFF' 'title_contrast = false
title_shadow = false'
shot
expect_none "$notwhite" "title_shadow = false still shaded the symbols" "$BUTTONS"

echo "the edge follows title_shadow on the title and the symbols, the title and symbols reverse only when their colour would vanish, and window mode reads the edge back only when asked"
