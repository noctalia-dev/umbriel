#!/usr/bin/env bash
# `color_source` takes the bar's fill from the window instead of the palette: "window" mirrors its top rows, "average"
# paints their mean. The window paints its first row green over a red body, so a bar sampling anything else shows red.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/adaptive-bar.log"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/adaptive-bar.png"
# The floating window is 480x300 at 173,109, so its 28 px bar covers 173..653 by 81..109. The title text is drawn
# transparent and the buttons paint nothing, so this crop inside the bar reads the fill alone.
readonly BAR="120x10+233+87"
# The whole bar: x 173..653 by y 81..109, inset by a pixel, covering the title and every button slot.
readonly BAR_FULL="474x26+175+82"

fail() {
  echo "$1"
  exit 1
}

# $1 color_source, $2 title colour (default transparent), $3 "closing" to enable a fade-out the checks can
# sample, $4 "buttons" to paint the button backgrounds, $5 rows of the window's edge to sample.
configure() {
  local source=$1 text=${2:-"#00000000"} closing=${3:-} buttons=${4:-} rows=${5:-1}
  {
    cat <<EOF
[general]
xwayland = false
show_cheatsheet = false
autostart = []

[output.HEADLESS-1]
scale = 1.0

[animation]
EOF
    if [[ $closing == closing ]]; then
      cat <<'EOF'
duration_ms = 1000
curve = "linear"

[animation.windows_out]
style = "fade"
EOF
    else
      echo 'enabled = false'
    fi
    cat <<EOF

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.title_bar]
mode = "forced"
height = 28
color_source = "$source"
color_source_height = $rows
button_background = $(if [[ $buttons == buttons ]]; then echo true; else echo false; fi)
button_glyph = "never"

[appearance.shadow]
enabled = false

[appearance.blur]
enabled = false

[colors.title_bar]
background = "#FF00FFFF"
background_unfocused = "#FF00FFFF"
text = "$text"
text_unfocused = "$text"
$(if [[ $buttons == buttons ]]; then printf 'close = "#FF0000FF"\nmaximize = "#FF0000FF"\nminimize = "#FF0000FF"'; fi)

[[window_rule]]
match.title = "^adaptive-bar\$"
default_floating = true
default_floating_size_px = { width = 480, height = 300 }
default_position = { x = 173, y = 109, anchor = "top_left" }
EOF
  } > "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

# Pixels in the bar crop matching a predicate.
count() { "$UMBRIEL_PIXEL_PROBE" "$FRAME" count "$1" "$BAR"; }
count_in() { "$UMBRIEL_PIXEL_PROBE" "$FRAME" count "$1" "$2"; }
pixel() { "$UMBRIEL_PIXEL_PROBE" "$FRAME" pixel "$1" "$2"; }

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

expect_none_in() {
  local predicate=$1 crop=$2 what=$3 found
  found=$(count_in "$predicate" "$crop")
  if ((found != 0)); then
    fail "$what: $found pixels match '$predicate' inside $crop"
  fi
}

expect_in() {
  local predicate=$1 crop=$2 minimum=$3 what=$4 found
  found=$(count_in "$predicate" "$crop")
  if ((found < minimum)); then
    fail "$what: only $found pixels match '$predicate' inside $crop"
  fi
}

green='g > 0.9 && r < 0.1 && b < 0.1'
red='r > 0.9 && g < 0.1 && b < 0.1'
blue='b > 0.9 && r < 0.1 && g < 0.1'
magenta='r > 0.9 && b > 0.9 && g < 0.1'

[[ -x $CLIENT ]] || fail "unmap client is not built"

configure window

mkfifo "$UMBRIEL_RUNTIME_DIR/adaptive-bar.fifo"
FILL_COLOR=0xFFFF0000 TOP_BAND_COLOR=0xFF00FF00 TOP_BAND_HEIGHT=1 REPAINT_ON_STDIN=1 \
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

# The sample height is live: one row is the green edge, three rows reach into the red body.
configure window "#00000000" "" "" 3
"$UMBRIEL" settle
grim "$FRAME"
expect_in "$red" "$BAR_FULL" 1000 "a three-row sample reaching into the window body"
configure window
"$UMBRIEL" settle
grim "$FRAME"
expect_none "$red" "the one-row sample kept to the window's top edge"

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

# The close animation draws the title above the fill: a snapshot that stacks the fill over it shows a flat smear.
window_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "adaptive-bar") | .id')
[[ -n $window_id && $window_id != null ]] || fail "the adaptive-bar window is gone before the close animation"
configure window "#FFFFFFFF" closing buttons
"$UMBRIEL" settle
grim "$FRAME"
expect_in 'r > 0.9 && g > 0.9 && b > 0.9' "$BAR_FULL" 40 "the title over the window-coloured fill"
"$UMBRIEL" clock-freeze > /dev/null
"$UMBRIEL" msg window-close "$window_id" > /dev/null
"$UMBRIEL" clock-advance 250 > /dev/null
grim "$FRAME"
expect_in 'r > 0.6 && g > 0.6 && b > 0.6' "$BAR_FULL" 40 "the title in the bar's close animation"
# The button backgrounds are painted under the fill in the live bar; the snapshot has to keep that order too.
expect_in 'r > 0.6 && g < 0.15 && b < 0.15' "$BAR_FULL" 40 "the button backgrounds in the bar's close animation"
"$UMBRIEL" clock-advance 1000 > /dev/null
"$UMBRIEL" settle

# The average takes one colour from the whole edge: a band that is green on one side and blue on the other must show the
# same blend at both ends of the bar. The smear shows the sides. This is what tells the two modes apart, and it is what
# keeps a tab row or a loading bar on that edge out of the colour.
readonly LOG2="$UMBRIEL_RUNTIME_DIR/adaptive-average.log"
configure average
# A fresh pipe: the first client's write end is closed with it.
mkfifo "$UMBRIEL_RUNTIME_DIR/adaptive-average.fifo"
FILL_COLOR=0xFFFF0000 TOP_BAND_COLOR=0xFF00FF00 TOP_BAND_SPLIT=0xFF0000FF TOP_BAND_HEIGHT=2 REPAINT_ON_STDIN=1 \
  "$CLIENT" adaptive-bar 480 300 < "$UMBRIEL_RUNTIME_DIR/adaptive-average.fifo" > "$LOG2" 2>&1 &
exec 4> "$UMBRIEL_RUNTIME_DIR/adaptive-average.fifo"
await_lines "$LOG2" mapped 1
"$UMBRIEL" settle
grim "$FRAME"
read -r left_r left_g left_b < <(pixel 200 84)
read -r right_r right_g right_b < <(pixel 620 84)
near() { (( $1 >= $2 - 12 && $1 <= $2 + 12 )); }
if ! near "$left_g" "$right_g" || ! near "$left_b" "$right_b" || ! near "$left_r" "$right_r"; then
  fail "an averaged bar was not uniform: left $left_r $left_g $left_b, right $right_r $right_g $right_b"
fi
# A blend of the green and blue halves, not either of them and not the palette.
if (( left_g < 60 || left_b < 60 || left_r > 40 )); then
  fail "an averaged bar did not blend the window edge: $left_r $left_g $left_b"
fi

# The same window in smear mode keeps the two sides distinct.
configure window
"$UMBRIEL" settle
grim "$FRAME"
read -r left_r left_g left_b < <(pixel 200 84)
read -r right_r right_g right_b < <(pixel 620 84)
if ! (( left_g > 200 && left_b < 40 && right_b > 200 && right_g < 40 )); then
  fail "a smeared bar did not keep the window edge: left $left_r $left_g $left_b, right $right_r $right_g $right_b"
fi

# Repaint while the bar is smearing, so the mean cached before this point is out of date.
printf 'B' >&4
await_lines "$LOG2" repainted 1
"$UMBRIEL" settle

# Switching to average is live on its own: an idle window must not wait for its next commit, and the mean must be the
# edge as it is now, not the one the bar last averaged.
configure average
"$UMBRIEL" settle
grim "$FRAME"
read -r left_r left_g left_b < <(pixel 200 84)
read -r right_r right_g right_b < <(pixel 620 84)
if ! near "$left_g" "$right_g" || ! (( left_b > 200 && left_g < 40 )); then
  fail "an averaged bar was stale after switching live: left $left_r $left_g $left_b, right $right_r $right_g $right_b"
fi

# And the mean follows further repaints rather than freezing on the colour it was first baked with.
printf 'R' >&4
await_lines "$LOG2" repainted 2
"$UMBRIEL" settle
grim "$FRAME"
read -r left_r left_g left_b < <(pixel 200 84)
read -r right_r right_g right_b < <(pixel 620 84)
if ! near "$left_r" "$right_r" || ! near "$left_b" "$right_b" || ! (( left_r > 100 && left_b > 100 && left_g < 40 )); then
  fail "an averaged bar did not follow a repaint: left $left_r $left_g $left_b, right $right_r $right_g $right_b"
fi

# A commit that damages only the body must not read the strip back: the average is a flat colour, so a readback there
# is pure cost, and it finishes the frame first. The compositor logs each readback, so the count is the observable.
readbacks() { rg -c 'averaged a [0-9]+x[0-9]+ strip read back' "$UMBRIEL_RUNTIME_DIR/compositor.log" || true; }
before=$(readbacks)
# A zero baseline would make the comparison vacuous: the earlier phases must have sampled at least once.
(( before > 0 )) || fail "no strip readback was logged, so the average never sampled the edge"
printf 'b' >&4
await_lines "$LOG2" repainted 3
"$UMBRIEL" settle
after=$(readbacks)
if (( after != before )); then
  fail "a body-only commit read the strip back: $before -> $after"
fi

# A transparent edge is a readable edge with no colour in it, so the palette comes back. A buffer that cannot be read
# at all is the other case: it keeps the last colour the window gave. The two must not share a path.
printf 't' >&4
await_lines "$LOG2" repainted 4
"$UMBRIEL" settle
grim "$FRAME"
expect "$magenta" 1000 "the palette colour under a transparent averaged edge"
expect_none 'g > 0.4 && g < 0.6 && b > 0.4 && b < 0.6' "the averaged colour survived a transparent edge"
