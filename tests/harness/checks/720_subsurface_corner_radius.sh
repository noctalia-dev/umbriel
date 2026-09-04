#!/usr/bin/env bash
# Content drawn through a wl_subsurface must follow the window corner radius. Firefox puts all of its chrome and web
# content into one full-window desynchronized subsurface, and rounding only the toplevel's own surface left that content
# square outside the arc. The client here paints its parent surface red and its full-window subsurface blue over a green
# backdrop, so a pixel just outside the corner arc reports which of the three the compositor drew.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/subsurface-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/subsurface-corner-radius.png"
readonly RADIUS=64

if [[ ! -x $CLIENT ]]; then
  echo "subsurface client not built at $CLIENT"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
duration_ms = 1

[colors]
backdrop = "#00FF00FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = $RADIUS
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" "subsurface-radius" > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "subsurface client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

# The layout box, not an assumed geometry: whatever else is mapped, the reported box is where this window is drawn.
window_box() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "subsurface-radius") | "\(.x) \(.y) \(.w) \(.h)"'
}
box=
for _ in $(seq 60); do
  box=$(window_box)
  [[ -n $box ]] && break
  sleep 0.1
done
if [[ -z $box ]]; then
  echo "subsurface-radius window never appeared in the window list"
  exit 1
fi
# Sample the box the layout settled on, after the (1 ms) open animation has run.
sleep 0.5
read -r win_x win_y win_w win_h <<< "$(window_box)"
grim "$SCREENSHOT"

# One sample reports green and blue at once: green is the compositor backdrop, blue is the client's subsurface.
sample() {
  magick "$SCREENSHOT" -crop "1x1+$1+$2" -format '%[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

# (3,3) from the window origin sits ~86 px from the arc centre at (RADIUS,RADIUS), well outside the 64 px arc, so a
# correctly rounded subsurface leaves the backdrop visible there. Unrounded, the square subsurface covers it in blue.
read -r corner_green corner_blue <<< "$(sample $((win_x + 3)) $((win_y + 3)))"
if (( corner_green < 200 || corner_blue > 60 )); then
  echo "window corner was not rounded: green=$corner_green blue=$corner_blue at ($((win_x + 3)),$((win_y + 3)))"
  exit 1
fi

# Just inside the arc, and at the window centre: the subsurface must still be drawn. Rounding that hides content, or a
# client that never presented its child surface, would pass the corner probe on its own.
read -r inside_green inside_blue <<< "$(sample $((win_x + RADIUS - 4)) $((win_y + RADIUS - 4)))"
if (( inside_blue < 200 )); then
  echo "subsurface missing just inside the corner arc: green=$inside_green blue=$inside_blue"
  exit 1
fi
read -r center_green center_blue <<< "$(sample $((win_x + win_w / 2)) $((win_y + win_h / 2)))"
if (( center_blue < 200 )); then
  echo "subsurface missing at the window centre: green=$center_green blue=$center_blue"
  exit 1
fi

echo "subsurface content follows the ${RADIUS}px corner radius: corner green=$corner_green blue=$corner_blue, inside blue=$inside_blue, centre blue=$center_blue"
