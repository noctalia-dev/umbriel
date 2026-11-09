#!/usr/bin/env bash
# A tile focused under a fullscreen window in its column is drawn above it, and goes back under the fullscreen window,
# and under floats once fullscreen ends, when focus leaves it. Untabbing a group with a fullscreen hidden tab shows the
# focused tab the same way, and toggling fullscreen there fullscreens that tab.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/tile-above-fullscreen.png"
readonly RED='r > 0.9 && g < 0.1 && b < 0.1'
readonly BLUE='r < 0.1 && g < 0.1 && b > 0.9'
readonly GREEN='r < 0.1 && g > 0.9 && b < 0.1'

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

msg() { "$UMBRIEL" msg "$@" > /dev/null; }

spawn() {
  RESIZE_FILL_COLOR="$2" "$CLIENT" "$1" 600 400 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    "$UMBRIEL" windows --json | jq -e --arg title "$1" 'any(.[]; .title == $title)' > /dev/null && return 0
    sleep 0.025
  done
  echo "window '$1' never appeared: $(cat "$UMBRIEL_RUNTIME_DIR/$1.log")"
  return 1
}

# Prints the size of what shows in a colour, as WIDTHxHEIGHT.
shown() {
  "$UMBRIEL" settle
  grim "$SHOT"
  "$UMBRIEL_PIXEL_PROBE" "$SHOT" bbox "$1" | awk '{ print $3 "x" $4 }'
}

spawn top 0xFFFF0000
spawn bottom 0xFF0000FF
msg window-consume-left
msg window-toggle-fullscreen
msg window-focus-up
if [[ $(shown "$RED") == 0x0 ]]; then
  echo "the focused top window stayed hidden under the fullscreen one: $("$UMBRIEL" windows --json)"
  exit 1
fi

msg window-focus-down
if [[ $(shown "$BLUE") != 1280x720 || $(shown "$RED") != 0x0 ]]; then
  echo "the fullscreen window did not cover the output again once focused: $("$UMBRIEL" windows --json)"
  exit 1
fi

msg window-toggle-fullscreen
spawn float 0xFFFF00FF
msg window-toggle-floating
read -r x y < <("$UMBRIEL" windows --json | jq -r '
  (.[] | select(.title == "float")) as $float | (.[] | select(.title == "top")) as $top
  | "\([$float.x, $top.x] | max + 10) \([$float.y, $top.y] | max + 10)"')
"$UMBRIEL" settle
grim "$SHOT"
if [[ $("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel "$x" "$y") != "255 0 255" ]]; then
  echo "the top window stayed above a float after fullscreen ended: $("$UMBRIEL" windows --json)"
  exit 1
fi
kill "$("$UMBRIEL" windows --json | jq '.[] | select(.title == "float") | .pid')"
"$UMBRIEL" msg window-focus "$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "bottom") | .id')" > /dev/null
msg window-toggle-fullscreen

spawn third 0xFF00FF00
msg window-consume-left
msg column-toggle-tabbed
msg column-focus-tab 3
msg column-toggle-tabbed
if [[ $(shown "$GREEN") == 0x0 ]]; then
  echo "the focused tab stayed hidden under a fullscreen tab after untabbing: $("$UMBRIEL" windows --json)"
  exit 1
fi

msg window-toggle-fullscreen
if [[ $(shown "$GREEN") != 1280x720 ]]; then
  echo "toggling fullscreen did not fullscreen the focused window shown above: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "a tile focused under a fullscreen window showed above it, and the fullscreen window covered the output again"
