#!/usr/bin/env bash
# scratchpad-window-cycle shows and hides on one key: it shows only the window after the one focused last, hides the pad
# while one of its windows has focus, and focuses the window on show when focus is elsewhere. It shares its order with
# scratchpad-window-show-next, and after the window focused last closes it resumes at the window after it. The
# -previous variant steps the other way.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/cycle.png"

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
  "$UMBRIEL" msg "$action" > /dev/null
  actual=$(shown)
  if [[ $actual != "$expected" ]]; then
    echo "after '$action' expected [$expected] on screen, got [$actual]"
    exit 1
  fi
}

expect_active() {
  local actual
  actual=$("$UMBRIEL" windows --json | jq -r '.[] | select(.active) | .title')
  if [[ $actual != "$1" ]]; then
    echo "$2: expected $1 to have focus, got [$actual]"
    exit 1
  fi
}

spawn plain 0xFFFFFFFF
spawn pad-red 0xFFFF0000
spawn pad-green 0xFF00FF00
green_pid=$!
spawn pad-blue 0xFF0000FF

# One key alternates show and hide, moving on to the next window each time it shows and wrapping at the end.
expect scratchpad-window-cycle red
expect_active pad-red "the window cycle showed"
expect scratchpad-window-cycle ""
expect scratchpad-window-cycle green
expect scratchpad-window-cycle ""
expect scratchpad-window-cycle blue
expect scratchpad-window-cycle ""
expect scratchpad-window-cycle red

# The previous variant hides the same way and steps the other way.
expect scratchpad-window-cycle-previous ""
expect scratchpad-window-cycle-previous blue
expect scratchpad-window-cycle-previous ""
expect scratchpad-window-cycle red

# It follows the order scratchpad-window-show-next steps through.
expect scratchpad-window-show-next green
expect scratchpad-window-cycle ""
expect scratchpad-window-cycle blue

# With focus on another window, it focuses the window on show instead of hiding it.
"$UMBRIEL" msg window-focus "$(window_of plain | jq -r .id)" > /dev/null
expect_active plain "focusing a window outside the pad"
expect scratchpad-window-cycle blue
expect_active pad-blue "cycle with the pad shown but not focused"

# Closing the window on show hides the pad; the next show is the window after it, not the first one.
expect scratchpad-window-show-previous green
kill "$green_pid"
for _ in $(seq 80); do
  [[ -z "$(window_of pad-green)" ]] && break
  sleep 0.025
done
[[ $(shown) == "" ]] || {
  echo "closing the window on show left [$(shown)] on screen"
  exit 1
}
expect scratchpad-window-cycle blue

echo "scratchpad-window-cycle showed and hid one window at a time on one key"
