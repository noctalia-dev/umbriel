#!/usr/bin/env bash
# While windows_resize.wait_for_redraw holds a reflow that moves focus, window rules matching is_focused keep showing
# what they showed before the move until the switch: here rule ring colours and the unfocused opacity. Opening a
# window holds the lone window focused and opaque; closing the focused tile holds the survivor unfocused and dimmed
# until it lands full width.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

# The compositor's fixed redraw wait (kRedrawWaitMs).
readonly TIMEOUT_MS=150
readonly SHOTS="$UMBRIEL_RUNTIME_DIR/resize-wait-focus-rules"
mkdir -p "$SHOTS"

cat >> "$UMBRIEL_CONFIG" <<EOF

[colors]
backdrop = "#000000FF"

[colors.border]
focused = "#FF00FFFF"
unfocused = "#000000FF"

[[window_rule]]
match.is_focused = true
border_color_focused = "#00FF00FF"

[[window_rule]]
match.is_focused = false
border_color_unfocused = "#0000FFFF"
opacity = 0.5

[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[layout]
mode = "dwindle"
gap = 40

[animation.windows_in]
enabled = false

[animation.windows_out]
enabled = false

[animation.border]
enabled = false

[animation.windows_opacity]
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
# The unfocused rule ring, drawn at the rule's half opacity, in the left half only: the opener fills blue on the right.
blue_left() {
  "$UMBRIEL_PIXEL_PROBE" "$1" count 'b > 0.4 && r < 0.1 && g < 0.1' "600x720+0+0"
}
# The red level inside the left-hand tile, which the lone window and later the survivor fill.
content() {
  "$UMBRIEL_PIXEL_PROBE" "$1" mean "100x100+300+300" | awk '{ print $1 }'
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
before_content=$(content "$SHOTS/before.png")
if ((before_green == 0)); then
  echo "the lone window has no rule-coloured focused ring before the open"
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
held_content=$(content "$SHOTS/held.png")
if ((held_green != before_green)); then
  echo "the held lone window's rule ring changed before the switch: $before_green green pixels, then $held_green"
  exit 1
fi
if ((held_content != before_content)); then
  echo "the held lone window's opacity changed before the switch: red $before_content, then $held_content"
  exit 1
fi

echo r >&"$control_fd"
await_lines "$log" 'resized' 2 0.025
"$UMBRIEL" clock-advance 1 > /dev/null
grim "$SHOTS/switched.png"
dimmed_content=$(content "$SHOTS/switched.png")
if ((dimmed_content >= before_content)); then
  echo "the unfocused survivor was not dimmed by the rule after the switch: red $dimmed_content"
  exit 1
fi

# Close the focused opener. The survivor grows to the full width, so the close is held for it too.
"$UMBRIEL" clock-advance 1500 > /dev/null
"$UMBRIEL" msg window-close "$(window_by_title focus-new | jq -r .id)" > /dev/null
await_lines "$log" 'resize-pending' 3 0.025
"$UMBRIEL" clock-advance 1 > /dev/null
"$UMBRIEL" clock-advance $((TIMEOUT_MS / 2)) > /dev/null
grim "$SHOTS/close-held.png"
# The survivor's frozen frame still shows the unfocused rule: its blue ring, and content dimmed below opaque.
if (($(blue_left "$SHOTS/close-held.png") == 0)); then
  echo "the held survivor lost its unfocused rule ring before the close switched"
  exit 1
fi
close_held_content=$(content "$SHOTS/close-held.png")
if ((close_held_content >= before_content)); then
  echo "the held survivor turned opaque before the close switched: red $close_held_content"
  exit 1
fi

echo r >&"$control_fd"
await_lines "$log" 'resized' 3 0.025
"$UMBRIEL" clock-advance 1 > /dev/null
grim "$SHOTS/close-switched.png"
if (($(green "$SHOTS/close-switched.png") != before_green)) || (($(content "$SHOTS/close-switched.png") != before_content)); then
  echo "the survivor did not land full width, focused and opaque in the switch"
  exit 1
fi

echo "focus rules kept what they showed until each held switch"
