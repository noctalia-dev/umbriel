#!/usr/bin/env bash
# Window rules carry their own frame, outer ring, corner radius, and shadow, so a
# client-side-decorated application can be left bare while every other window
# keeps the global decoration. The check reads the pixels just outside two
# identical floating windows: the one whose rule sets border_width = 0 and
# shadow = false shows the background where the other shows the frame, the outer
# ring its rule adds, and its drop shadow.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/rule-decoration.png"

# A white desktop and a red or green frame make both effects readable as whole
# pixels, and the desktop color is [colors] backdrop. The global corner radius
# stays high so the framed window's corners are really rounded: that is what the
# bare window's square corner has to differ from. Samples beside a window's
# midpoint stay clear of the corners either way.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#FFFFFFFF"

[colors.border]
focused = "#FF0000FF"
unfocused = "#00FF00FF"
outer = "#FF0000FF"

[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 12

[[window_rule]]
match.title = "^bare-window$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 80, y = 120, anchor = "top_left" }
border_width = 0
corner_radius = 0
shadow = false

[[window_rule]]
match.title = "^framed-window$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 620, y = 120, anchor = "top_left" }
outer_border_width = 4
EOF
"$UMBRIEL" msg config-reload > /dev/null

window_of() {
  "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'
}

wait_mapped() {
  for _ in $(seq 60); do
    [[ -n $(window_of "$1") ]] && return 0
    sleep 0.05
  done
  echo "$1 never mapped: $("$UMBRIEL" windows --json)"
  exit 1
}

# Window opens and the config reload animate; settle waits for every timeline to reach rest before the grab.
shot_settled() {
  "$UMBRIEL" settle
  grim -o HEADLESS-1 "$SCREENSHOT"
}

"$CLIENT" bare-window > "$UMBRIEL_RUNTIME_DIR/rule-decoration-bare.log" 2>&1 &
wait_mapped bare-window
"$CLIENT" framed-window > "$UMBRIEL_RUNTIME_DIR/rule-decoration-framed.log" 2>&1 &
wait_mapped framed-window

BARE=$(window_of bare-window)
FRAMED=$(window_of framed-window)

# The rules must have been selected, so the samples below read the edges they
# claim to read: both windows took the floating size and position they asked for.
for entry in "bare-window:$BARE" "framed-window:$FRAMED"; do
  title=${entry%%:*}
  window=${entry#*:}
  if [[ $(jq -r '.w' <<< "$window") != 400 || $(jq -r '.h' <<< "$window") != 300 ]]; then
    echo "$title did not adopt its rule size: $window"
    exit 1
  fi
done
if [[ $(jq -r '.x' <<< "$BARE") != 80 || $(jq -r '.x' <<< "$FRAMED") != 620
  || $(jq -r '.y' <<< "$BARE") != 120 || $(jq -r '.y' <<< "$FRAMED") != 120 ]]; then
  echo "rule positions were not applied: bare $BARE framed $FRAMED"
  exit 1
fi

shot_settled

read -r screen_w screen_h <<< "$(magick identify -format '%w %h' "$SCREENSHOT")"

# One pixel of the screenshot, as RRGGBB, at an offset from a window's own
# top-left corner.
hex_at() {
  local x=$(($(jq -r '.x' <<< "$1") + $2))
  local y=$(($(jq -r '.y' <<< "$1") + $3))
  if ((x < 0 || y < 0 || x >= screen_w || y >= screen_h)); then
    echo "sample ($x,$y) falls outside the ${screen_w}x${screen_h} output" >&2
    exit 1
  fi
  magick "$SCREENSHOT" -alpha off -crop "1x1+$x+$y" +repage -depth 8 txt:- |
    sed -n 's/.*#\([0-9A-F]\{6\}\).*/\1/p' | head -1
}

expect_frame() {
  local color
  color=$(hex_at "$1" "$2" "$3")
  if [[ $color != FF0000 && $color != 00FF00 ]]; then
    echo "$4: expected the frame color, read #$color"
    exit 1
  fi
}

expect_background() {
  local color
  color=$(hex_at "$1" "$2" "$3")
  if [[ $color != FFFFFF ]]; then
    echo "$4: expected the plain background, read #$color"
    exit 1
  fi
}

expect_shadow() {
  local color
  color=$(hex_at "$1" "$2" "$3")
  if [[ $color == FFFFFF ]]; then
    echo "$4: expected a drop shadow, read the plain background"
    exit 1
  fi
}

# The client paints blue and green columns across its whole surface, so a corner
# pixel that is neither is a corner the compositor rounded away.
expect_content() {
  local color
  color=$(hex_at "$1" "$2" "$3")
  if [[ $color != 0000FF && $color != 00FF00 ]]; then
    echo "$4: expected client content, read #$color"
    exit 1
  fi
}

# One assertion per key, each one reachable by the key it covers alone, so a
# failure names the behavior that regressed.

# corner_radius: the global radius rounds a window's corners away unless its rule
# squares them, which shows at the client surface's own corner pixel.
expect_content "$BARE" 0 0 "bare window's corner"

# shadow: it is offset two pixels down and blurs over ten, so a few pixels past a
# window's ring separate a shadow from the plain background: six below the bare
# window, twelve below the framed one and its eight-pixel ring.
expect_shadow "$FRAMED" 200 312 "framed window's shadow"
expect_background "$BARE" 200 306 "bare window's shadow area"

# border_width: the global frame is four pixels wide, so two pixels outside the
# client edge are still inside it. The bare window draws nothing there.
expect_background "$BARE" -2 150 "bare window's left edge"
expect_frame "$FRAMED" -2 150 "framed window's left frame"

# outer_border_width: the global outer ring is off, so six pixels outside the client edge lie past the global frame.
# The framed window's rule adds a four-pixel outer ring there; the bare window still draws nothing.
expect_background "$BARE" -6 150 "bare window's outer edge"
if [[ $(hex_at "$FRAMED" -6 150) != FF0000 ]]; then
  echo "framed window's outer ring: expected #FF0000, read #$(hex_at "$FRAMED" -6 150)"
  exit 1
fi

# The override works in the other direction too: with the global switch off, a rule
# that sets shadow = true still draws one, while a window without the rule stays
# bare. That is the per-window shadow #230 asks for, and an implementation that
# merely ANDs the global switch with the rule value fails here.
printf '\n[appearance.shadow]\nenabled = false\n' >> "$UMBRIEL_CONFIG"
sed -i 's/^shadow = false$/shadow = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
shot_settled
expect_shadow "$BARE" 200 306 "shadow = true under a globally disabled shadow"
expect_background "$FRAMED" 200 312 "window without the rule under the same global switch"

echo "border_width = 0, corner_radius = 0, and shadow = false left one window bare while the other kept the global frame and drew its rule's outer ring, and shadow = true drew a shadow where the global switch was off"
