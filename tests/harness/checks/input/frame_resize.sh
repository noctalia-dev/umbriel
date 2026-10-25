#!/usr/bin/env bash
# Floating frame grips survive title bar changes without taking client input.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
title_bar_config
title=ssd-float-resize
log=$UMBRIEL_RUNTIME_DIR/resize.log
env XDG_DECORATION=server LOG_POINTER=1 "$UMBRIEL_FRACTIONAL_CLIENT" "$title" 480 300 > "$log" 2>&1 &
title_bar_wait_box "$title" 480x300+173+109
"$UMBRIEL" settle > /dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 170 250 press 272 move 130 250 release 272
title_bar_wait_box "$title" 520x300+133+109
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 130 83 press 272 move 110 63 release 272
title_bar_wait_box "$title" 540x320+113+89

read_box() {
  read -r x y w h < <(title_bar_window "$title" | jq -r '[.x, .y, .w, .h] | @tsv')
}

set_title_bar() {
  sed -i "/^\[appearance.title_bar\]$/ { n; s/mode = \".*\"/mode = \"$1\"/; }" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  title_bar_wait_field "$title" title_bar "$([[ $1 == on ]] && echo true || echo false)"
  "$UMBRIEL" settle > /dev/null
}

resize_left() {
  read_box
  "$UMBRIEL_POINTER_CLIENT" 1280 720 \
    move "$((x - 3))" "$((y + h / 2))" press 272 move "$((x - 43))" "$((y + h / 2))" release 272
  title_bar_wait_box "$title" "$((w + 40))x$h+$((x - 40))+$y"
}

# Removing the title bar from an existing frame must leave live side and corner grips.
set_title_bar off
resize_left
read_box
"$UMBRIEL_POINTER_CLIENT" 1280 720 \
  move "$((x - 3))" "$((y - 3))" press 272 move "$((x - 23))" "$((y - 23))" release 272
title_bar_wait_box "$title" "$((w + 20))x$((h + 20))+$((x - 20))+$((y - 20))"

# Showing the title bar again must preserve the frame's input region.
set_title_bar on
resize_left
if rg -q '^pointer-button ' "$log"; then
  echo "frame resize input leaked to the application: $(cat "$log")"
  exit 1
fi

# An ordinary content drag next to the edge belongs to the application.
read_box
"$UMBRIEL_POINTER_CLIENT" 1280 720 \
  move "$((x + 1))" "$((y + h / 2))" press 272 move "$((x + 21))" "$((y + h / 2))" release 272
"$UMBRIEL" settle > /dev/null
title_bar_wait_box "$title" "${w}x$h+$x+$y"
await_lines "$log" 'pointer-button 272 1' 1
await_lines "$log" 'pointer-button 272 0' 1

# A frame that was resizable while floating must stop offering grips when tiled.
"$UMBRIEL" msg "window-toggle-floating:$(title_bar_window "$title" | jq -r .id)" > /dev/null
title_bar_wait_field "$title" floating false
"$UMBRIEL" settle > /dev/null
read_box
"$UMBRIEL_POINTER_CLIENT" 1280 720 \
  move "$((x + w + 3))" "$((y + h / 2))" press 272 move "$((x + w + 43))" "$((y + h / 2))" release 272
"$UMBRIEL" settle > /dev/null
title_bar_wait_box "$title" "${w}x$h+$x+$y"

# A new floating window must get grips even when it never had a title bar.
set_title_bar off
title=ssd-float-no-title
log=$UMBRIEL_RUNTIME_DIR/no-title.log
env XDG_DECORATION=server LOG_POINTER=1 "$UMBRIEL_FRACTIONAL_CLIENT" "$title" 480 300 > "$log" 2>&1 &
title_bar_wait_field "$title" floating true
title_bar_wait_field "$title" title_bar false
"$UMBRIEL" settle > /dev/null
resize_left
if rg -q '^pointer-button ' "$log"; then
  echo "titleless frame resize input leaked to the application: $(cat "$log")"
  exit 1
fi

# Client-side content retains input inside its geometry even with offset surface margins.
title=ssd-float-client-margin
log=$UMBRIEL_RUNTIME_DIR/client-margin.log
env XDG_DECORATION=client LOG_POINTER=1 "$UMBRIEL_FRACTIONAL_CLIENT" "$title" 480 300 12 > "$log" 2>&1 &
title_bar_wait_field "$title" floating true
title_bar_wait_field "$title" title_bar false
"$UMBRIEL" settle > /dev/null
read_box
"$UMBRIEL_POINTER_CLIENT" 1280 720 \
  move "$((x + 1))" "$((y + h / 2))" press 272 move "$((x + 21))" "$((y + h / 2))" release 272
"$UMBRIEL" settle > /dev/null
title_bar_wait_box "$title" "${w}x$h+$x+$y"
await_lines "$log" 'pointer-button 272 1' 1
await_lines "$log" 'pointer-button 272 0' 1
echo "floating frame grips survive title bar changes and preserve client-owned input"
