#!/usr/bin/env bash
# Maximizing a tiled window to the usable-area edges must pass through the shared size animation instead of snapping.
# A solid client on the black headless output makes its presented width measurable before, during, and after the action.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/maximize-to-edges-animation-client.log"
readonly BEFORE="$UMBRIEL_RUNTIME_DIR/maximize-to-edges-before.png"
readonly DURING="$UMBRIEL_RUNTIME_DIR/maximize-to-edges-during.png"
readonly AFTER="$UMBRIEL_RUNTIME_DIR/maximize-to-edges-after.png"

presented_width() {
  magick "$1" -alpha off -crop 1280x1+0+360 +repage -fuzz 2% \
    -fill black +opaque '#5577AA' -fill white -opaque '#5577AA' -colorspace gray \
    -format '%[fx:round(w*mean)]' info:
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 2000

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[layout.scrolling]
default_width_fraction = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" "maximize-to-edges-animation" 1280 720 > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "maximize-to-edges client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

sleep 2.1
grim "$BEFORE"
before_width=$(presented_width "$BEFORE")

"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
sleep 0.25
grim "$DURING"
during_width=$(presented_width "$DURING")

sleep 2.1
grim "$AFTER"
after_width=$(presented_width "$AFTER")

if (( after_width - before_width < 300 )); then
  echo "maximize-to-edges did not substantially widen the window: before=$before_width after=$after_width"
  exit 1
fi
if (( during_width <= before_width + 10 || during_width >= after_width - 10 )); then
  echo "maximize-to-edges snapped instead of presenting an intermediate width: before=$before_width during=$during_width after=$after_width"
  exit 1
fi

echo "maximize-to-edges animated through an intermediate width: $before_width -> $during_width -> $after_width"
