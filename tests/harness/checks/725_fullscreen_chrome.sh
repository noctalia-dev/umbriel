#!/usr/bin/env bash
# xwayland-satellite requests fullscreen before the initial surface commit when an X11 game window already matches an
# output. Umbriel must carry that pending request into the initial configure so the game maps as a square, borderless
# fullscreen surface instead of an output-sized rounded tile.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/subsurface-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-chrome-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/fullscreen-chrome.png"

if [[ ! -x $CLIENT ]]; then
  echo "subsurface client not built at $CLIENT"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[appearance]
border_width = 12
outer_border_width = 0
corner_radius = 64
EOF
"$UMBRIEL" msg config-reload > /dev/null

env INITIAL_FULLSCREEN=1 "$CLIENT" fullscreen-chrome > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "initial-fullscreen client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
first_configure=$(grep -m1 '^first-configure ' "$CLIENT_LOG" || true)
if [[ $first_configure != "first-configure 1280 720 fullscreen" ]]; then
  echo "initial configure was not output-sized fullscreen: ${first_configure:-missing}"
  exit 1
fi

window_box() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "fullscreen-chrome") | "\(.x) \(.y) \(.w) \(.h)"'
}

box=
for _ in $(seq 60); do
  box=$(window_box)
  if [[ $box =~ ^-?[0-9]+\ -?[0-9]+\ 1280\ 720$ ]]; then
    break
  fi
  sleep 0.1
done
if [[ ! $box =~ ^-?[0-9]+\ -?[0-9]+\ 1280\ 720$ ]]; then
  echo "fullscreen client did not accept the output size: ${box:-missing}"
  exit 1
fi
read -r win_x win_y _ _ <<< "$box"

sleep 0.25
grim "$SCREENSHOT"

sample() {
  magick "$SCREENSHOT" -crop "1x1+$1+$2" -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

read -r corner_red corner_green corner_blue <<< "$(sample $((win_x + 16)) $((win_y + 16)))"
if (( corner_red > 60 || corner_green > 60 || corner_blue < 200 )); then
  echo "fullscreen corner contains compositor chrome: red=$corner_red green=$corner_green blue=$corner_blue"
  exit 1
fi

read -r center_red center_green center_blue <<< "$(sample $((win_x + 640)) $((win_y + 360)))"
if (( center_red > 60 || center_green > 60 || center_blue < 200 )); then
  echo "fullscreen client content missing at output centre: red=$center_red green=$center_green blue=$center_blue"
  exit 1
fi

echo "an initial fullscreen request maps as a square, borderless fullscreen surface"
