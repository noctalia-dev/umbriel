#!/usr/bin/env bash
# scratchpad-show-next / scratchpad-show-previous show one scratchpad window at a time and cycle through the rest,
# wrapping at the ends. From every window shown, they keep the one after the focused window. scratchpad-toggle hides
# the pad however it was shown, window-focus shows the window it names, and closing the window on show hides the pad.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BTN_LEFT=272
readonly FRAME="$UMBRIEL_RUNTIME_DIR/show-step.png"

if [[ ! -x $CLIENT ]]; then
  echo "unmap client is not built"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
dim = 0.0
blur = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[[window_rule]]
match.title = "^pad-"
default_scratchpad = "default"
default_floating_size_px = { width = 300, height = 200 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

window_of() { "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'; }

spawn() {
  local title=$1 color=$2
  FILL_COLOR="$color" "$CLIENT" "$title" 300 200 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 80); do
    [[ -n "$(window_of "$title")" ]] && return 0
    sleep 0.025
  done
  echo "window '$title' never appeared: $(cat "$UMBRIEL_RUNTIME_DIR/$title.log")"
  return 1
}

# Which of the three colours are on screen, as a space-separated list.
shown() {
  "$UMBRIEL" settle
  grim "$FRAME"
  local visible=() w h
  read -r _ _ w h < <("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox 'r > 0.9 && g < 0.1 && b < 0.1')
  ((w > 0 && h > 0)) && visible+=(red)
  read -r _ _ w h < <("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox 'g > 0.9 && r < 0.1 && b < 0.1')
  ((w > 0 && h > 0)) && visible+=(green)
  read -r _ _ w h < <("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox 'b > 0.9 && r < 0.1 && g < 0.1')
  ((w > 0 && h > 0)) && visible+=(blue)
  echo "${visible[*]}"
}

expect() {
  local action=$1 expected=$2 actual
  if [[ -n $action ]]; then
    "$UMBRIEL" msg "$action" > /dev/null
  fi
  actual=$(shown)
  if [[ $actual != "$expected" ]]; then
    echo "after '${action:-setup}' expected [$expected] on screen, got [$actual]"
    exit 1
  fi
}

spawn pad-red 0xFFFF0000
red_pid=$!
spawn pad-green 0xFF00FF00
green_pid=$!
spawn pad-blue 0xFF0000FF
blue_pid=$!
expect "" ""

# One at a time, in order, wrapping at both ends.
expect scratchpad-show-next red
expect scratchpad-show-next green
expect scratchpad-show-next blue
expect scratchpad-show-next red
expect scratchpad-show-previous blue

# scratchpad-toggle hides a pad showing one window, and shows every window again. They open centred on each other, so
# two are dragged aside to show all three at once; dragging a scratchpad window keeps it in the scratchpad.
expect scratchpad-toggle ""
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" settle
"$POINTER" 1280 720 move 640 360 mod logo press "$BTN_LEFT" move 400 300 move 250 160 release "$BTN_LEFT" mod none
"$POINTER" 1280 720 move 640 360 mod logo press "$BTN_LEFT" move 800 420 move 1030 560 release "$BTN_LEFT" mod none
expect "" "red green blue"

# From every window shown, the step keeps only the window after the focused one.
focused=$("$UMBRIEL" windows --json | jq -r '.[] | select(.active) | .title')
case $focused in
  pad-red) after=green ;;
  pad-green) after=blue ;;
  *) after=red ;;
esac
expect scratchpad-show-next "$after"

# window-focus on a hidden member shows that member in place of the one on show.
hidden=red
[[ $after == red ]] && hidden=blue
"$UMBRIEL" msg "window-focus:$(window_of "pad-$hidden" | jq -r .id)" > /dev/null
expect "" "$hidden"

# Closing the window on show hides the pad rather than revealing the rest.
if [[ $hidden == red ]]; then kill "$red_pid"; else kill "$blue_pid"; fi
for _ in $(seq 80); do
  [[ -z "$(window_of "pad-$hidden")" ]] && break
  sleep 0.025
done
expect "" ""

kill "$red_pid" "$green_pid" "$blue_pid" 2> /dev/null || true
echo "scratchpad-show-next/previous showed one window at a time and cycled through the rest"
