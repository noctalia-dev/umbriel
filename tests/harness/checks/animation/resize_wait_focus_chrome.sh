#!/usr/bin/env bash
# While windows_resize.wait_for_redraw holds the reflow an opening window causes, the screen keeps showing the lone
# window exactly as it was before the open: its focused border and its shadow, not the unfocused ring focus moving to
# the still-hidden opener would give it, and not a second shadow stacked under the frozen frame. Closing the focused
# tile is the same switch the other way: the survivor's ring stays unfocused until it lands, focused, at full width.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

# The compositor's fixed redraw wait (kRedrawWaitMs).
readonly TIMEOUT_MS=150
readonly GAP=40
readonly SHOTS="$UMBRIEL_RUNTIME_DIR/resize-wait-focus"
mkdir -p "$SHOTS"

cat >> "$UMBRIEL_CONFIG" <<EOF

[colors]
backdrop = "#000000FF"
shadow = "#FFFFFFFF"

[colors.border]
focused = "#00FF00FF"
unfocused = "#000000FF"

[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = true
softness = 8
offset_x = 0
offset_y = 0

[layout]
mode = "dwindle"
gap = $GAP

[animation.windows_in]
enabled = false

[animation.windows_out]
enabled = false

[animation.border]
enabled = false

[animation.windows_move]
enabled = true

[animation.windows_resize]
enabled = false
wait_for_redraw = true
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" clock-freeze

green() {
  "$UMBRIEL_PIXEL_PROBE" "$1" count 'g > 0.9 && r < 0.1 && b < 0.1'
}

# The shadow's brightness in the gap left of the lone window, and in the gap above the opener's tile, where nothing
# but shadows draw.
shadow_level() {
  "$UMBRIEL_PIXEL_PROBE" "$1" mean "4x200+$((GAP - 6))+260" | awk '{ print $1 }'
}
opener_shadow_level() {
  "$UMBRIEL_PIXEL_PROBE" "$1" mean "400x4+760+$((GAP - 6))" | awk '{ print $1 }'
}

fifo="$UMBRIEL_RUNTIME_DIR/focus-control"
log="$UMBRIEL_RUNTIME_DIR/focus-lone.log"
mkfifo "$fifo"
exec {control_fd}<>"$fifo"
FILL_COLOR=0xFFFF0000 FOLLOW_CONFIGURES=1 RESIZE_ON_STDIN=1 \
  "$UMBRIEL_UNMAP_CLIENT" focus-lone 1280 720 < "$fifo" > "$log" 2>&1 &
await_window focus-lone
await_lines "$log" 'resize-pending' 1 0.025
echo r >&"$control_fd"
await_lines "$log" 'resized' 1 0.025
"$UMBRIEL" clock-advance 1500 > /dev/null
grim "$SHOTS/before.png"
before_green=$(green "$SHOTS/before.png")
before_shadow=$(shadow_level "$SHOTS/before.png")
before_opener_shadow=$(opener_shadow_level "$SHOTS/before.png")
if ((before_green == 0)); then
  echo "the lone window has no focused border before the open"
  exit 1
fi

FILL_COLOR=0xFF0000FF RESIZE_FILL_COLOR=0xFF0000FF \
  "$UMBRIEL_UNMAP_CLIENT" focus-new 1280 720 > "$UMBRIEL_RUNTIME_DIR/focus-new.log" 2>&1 &
await_window focus-new
await_lines "$log" 'resize-pending' 2 0.025
"$UMBRIEL" clock-advance 1 > /dev/null
"$UMBRIEL" clock-advance $((TIMEOUT_MS / 2)) > /dev/null
grim "$SHOTS/held.png"
held_green=$(green "$SHOTS/held.png")
held_shadow=$(shadow_level "$SHOTS/held.png")
held_opener_shadow=$(opener_shadow_level "$SHOTS/held.png")
if ((held_green != before_green)); then
  echo "the held lone window's focused border changed before the switch: $before_green green pixels, then $held_green"
  exit 1
fi
if awk -v a="$before_shadow" -v b="$held_shadow" 'BEGIN { d = a - b; exit !(d > 2 || d < -2) }'; then
  echo "the held lone window's shadow changed before the switch: $before_shadow, then $held_shadow"
  exit 1
fi
if awk -v a="$before_opener_shadow" -v b="$held_opener_shadow" 'BEGIN { d = a - b; exit !(d > 2 || d < -2) }'; then
  echo "the hidden opener's shadow showed before the switch: $before_opener_shadow, then $held_opener_shadow"
  exit 1
fi

echo r >&"$control_fd"
await_lines "$log" 'resized' 2 0.025
"$UMBRIEL" clock-advance 1 > /dev/null
grim "$SHOTS/switched.png"
# Focus is on the opener now: its ring is the only green one, and it sits in the right-hand tile.
read -r x _ w _ <<< "$("$UMBRIEL_PIXEL_PROBE" "$SHOTS/switched.png" bbox 'g > 0.9 && r < 0.1 && b < 0.1')"
if ((w == 0 || x < 640)); then
  echo "the focused ring did not move to the opener with the switch: green at x=$x w=$w"
  exit 1
fi

# Close the focused opener. The survivor must grow to the full width, so the close is held for it too.
"$UMBRIEL" clock-advance 1500 > /dev/null
"$UMBRIEL" msg window-close "$(window_by_title focus-new | jq -r .id)" > /dev/null
await_lines "$log" 'resize-pending' 3 0.025
"$UMBRIEL" clock-advance 1 > /dev/null
"$UMBRIEL" clock-advance $((TIMEOUT_MS / 2)) > /dev/null
grim "$SHOTS/close-held.png"
# Only the closed tile's frozen frame, still on the right, shows a focused ring.
read -r x _ w _ <<< "$("$UMBRIEL_PIXEL_PROBE" "$SHOTS/close-held.png" bbox 'g > 0.9 && r < 0.1 && b < 0.1')"
if ((w == 0 || x < 640)); then
  echo "focus moved to the survivor's frozen frame before the close switched: green at x=$x w=$w"
  exit 1
fi

echo r >&"$control_fd"
await_lines "$log" 'resized' 3 0.025
"$UMBRIEL" clock-advance 1 > /dev/null
grim "$SHOTS/close-switched.png"
switched_green=$(green "$SHOTS/close-switched.png")
if ((switched_green != before_green)); then
  echo "the survivor did not land full width and focused in the switch: $switched_green green pixels, expected $before_green"
  exit 1
fi

echo "held opens and closes kept every window's ring and shadow until the switch, then moved focus with it"
