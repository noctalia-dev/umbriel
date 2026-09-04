#!/usr/bin/env bash
# A game bridge can request fullscreen after its initial windowed configure but before attaching the first buffer. The
# mapped view must use that pending fullscreen state when deciding whether to create rounded window chrome.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-pending-map-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/fullscreen-pending-map.png"

if [[ ! -x $CLIENT ]]; then
  echo "subsurface client not built at $CLIENT"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[colors]
backdrop = "#FF0000FF"

[colors.border]
focused = "#00FF00FF"
unfocused = "#00FF00FF"

[appearance]
border_width = 12
outer_border_width = 0
corner_radius = 64
EOF
"$UMBRIEL" msg config-reload > /dev/null

env FULLSCREEN_BEFORE_MAP=1 TRANSPARENT_CONTENT=1 \
  "$CLIENT" fullscreen-pending-map > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "pending-fullscreen client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
first_configure=$(grep -m1 '^first-configure ' "$CLIENT_LOG" || true)
if [[ ! $first_configure =~ ^first-configure\ [0-9]+\ [0-9]+\ windowed$ ]]; then
  echo "client did not begin from a windowed configure: ${first_configure:-missing}"
  exit 1
fi

window_box() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "fullscreen-pending-map") | "\(.x) \(.y) \(.w) \(.h)"'
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
  echo "pending-fullscreen client did not accept the output size: ${box:-missing}"
  exit 1
fi
read -r win_x win_y _ _ <<< "$box"

sleep 0.25
grim "$SCREENSHOT"

sample() {
  magick "$SCREENSHOT" -crop "1x1+$1+$2" -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

# Count the exact configured border colour rather than assuming which corner pixel lands inside the antialiased ring.
# The transparent fullscreen client and red backdrop introduce no other green pixels.
green_pixels=$(magick "$SCREENSHOT" -format %c histogram:info: | awk '
  $3 == "#00FF00" { gsub(":", "", $1); print $1; found=1 }
  END { if (!found) print 0 }
')
if (( green_pixels > 0 )); then
  echo "pending fullscreen map retained green window chrome: pixels=$green_pixels"
  exit 1
fi

read -r center_red center_green center_blue <<< "$(sample $((win_x + 640)) $((win_y + 360)))"
if (( center_red < 200 || center_green > 60 || center_blue > 60 )); then
  echo "fullscreen client content missing at output centre: red=$center_red green=$center_green blue=$center_blue"
  exit 1
fi

echo "a pending fullscreen request maps without rounded window chrome"
