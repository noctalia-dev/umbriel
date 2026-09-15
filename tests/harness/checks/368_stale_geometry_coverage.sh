#!/usr/bin/env bash
# Electron acks a configure, redraws at the configured size, and leaves set_window_geometry at its previous size, for
# good. Presenting that stale box crops away the content the client just drew and leaves the window occupying a
# fraction of its tile, with no way to resize it back. Such a window must be presented, and reported, at the size it
# was configured to whenever its surface holds those pixels.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/stale-geometry-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/stale-geometry.png"
# The child surface covers the whole window box with opaque blue.
readonly CONTENT_COLOR=0000FF

if [[ ! -x $CLIENT ]]; then
  echo "subsurface client is not built"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title stale-geometry ".[] | select(.title == \$title) | .$1"
}

pixel_color() {
  magick "$SCREENSHOT" -alpha off -format "%[hex:p{$1,$2}]" info:
}

# "drew <size> geometry <size>": what the client painted, and the box it declared once and never updated.
drawn_size() {
  sed -n 's/^drew \([0-9]*x[0-9]*\) geometry [0-9]*x[0-9]*$/\1/p' "$CLIENT_LOG" | tail -1
}

pinned_size() {
  sed -n 's/^drew [0-9]*x[0-9]* geometry \([0-9]*x[0-9]*\)$/\1/p' "$CLIENT_LOG" | tail -1
}

# The first window in a scrolling column keeps the width it maps with, so the client pins its geometry to a narrow box
# before the layout ever widens it.
STALE_GEOMETRY=1 "$CLIENT" stale-geometry 400 300 > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -qx mapped "$CLIENT_LOG" 2>/dev/null && break
  sleep 0.1
done
pinned=$(pinned_size)
if ! grep -qx mapped "$CLIENT_LOG" || [[ -z $pinned ]]; then
  echo "client never mapped with a pinned geometry: $(tr '\n' '|' < "$CLIENT_LOG")"
  exit 1
fi

# Full width is a configure the client answers with a wider buffer while its declared geometry stays behind.
"$UMBRIEL" msg window-toggle-maximize > /dev/null
sleep 0.5
drawn=$(drawn_size)
if [[ $(pinned_size) != "$pinned" ]]; then
  echo "client stopped pinning its geometry, so this check observes nothing: $(tr '\n' '|' < "$CLIENT_LOG")"
  exit 1
fi
if [[ $drawn == "$pinned" ]]; then
  echo "maximize did not configure a size past the pinned box: $(tr '\n' '|' < "$CLIENT_LOG")"
  exit 1
fi

# The listing reports what the window covers, not the box the client forgot to update.
reported="$(field_of w)x$(field_of h)"
if [[ $reported != "$drawn" ]]; then
  echo "windows listing reports $reported for a window drawn at $drawn, pinned at $pinned"
  exit 1
fi

x=$(field_of x)
y=$(field_of y)
pinned_w=${pinned%x*}
drawn_w=${drawn%x*}
grim -o HEADLESS-1 "$SCREENSHOT"

# Inside the pinned box the content has always been drawn; past it is the region the stale box would crop.
control=$(pixel_color "$((x + pinned_w / 2))" "$((y + 100))")
if [[ $control != "$CONTENT_COLOR" ]]; then
  echo "content missing inside the client's own geometry box: $control"
  exit 1
fi
for column in $((x + pinned_w + 10)) $((x + drawn_w - 1)); do
  beyond=$(pixel_color "$column" "$((y + 100))")
  if [[ $beyond != "$CONTENT_COLOR" ]]; then
    echo "window cropped to its stale ${pinned} geometry: pixel at ${column},$((y + 100)) is $beyond"
    exit 1
  fi
done

echo "a window whose geometry lags its configure is presented and reported at ${drawn}, not cropped to ${pinned}"
