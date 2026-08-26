#!/usr/bin/env bash
# Closing a window in overview must retain a frozen card and fade it through the same close-snapshot owner used on the
# normal desktop. A solid client over a black overview makes the lifecycle measurable at the card center: visible
# before unmap, still visible immediately afterwards, then gone after the configured half-duration close fade.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/overview-close-fade-client.log"
readonly BEFORE="$UMBRIEL_RUNTIME_DIR/overview-close-before.png"
readonly DURING="$UMBRIEL_RUNTIME_DIR/overview-close-during.png"
readonly AFTER="$UMBRIEL_RUNTIME_DIR/overview-close-after.png"

sample_blue() {
  magick "$1" -crop 20x20+630+350 -format '%[fx:round(255*mean.b)]' info:
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1000

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
backdrop_color = "#000000FF"

[overview]
zoom = 0.5
background_tint = "#000000FF"
workspace_background = "#000000FF"
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" "overview-close-fade" 1200 700 > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "overview close client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

window_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "overview-close-fade") | .id')
if [[ -z $window_id ]]; then
  echo "could not resolve overview close client: $("$UMBRIEL" windows --json)"
  exit 1
fi

"$UMBRIEL" msg overview-open > /dev/null
sleep 1.1
grim "$BEFORE"
before_blue=$(sample_blue "$BEFORE")
if (( before_blue < 80 )); then
  echo "overview card was not visible before close: blue=$before_blue"
  exit 1
fi

"$UMBRIEL" msg "window-close:$window_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.01
done
if ! grep -q '^unmapped$' "$CLIENT_LOG"; then
  echo "overview close client never unmapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
grim "$DURING"
during_blue=$(sample_blue "$DURING")
if (( during_blue < 20 )); then
  echo "overview card disappeared instead of beginning a close fade: before=$before_blue during=$during_blue"
  exit 1
fi

sleep 0.7
grim "$AFTER"
after_blue=$(sample_blue "$AFTER")
if (( after_blue > 10 )); then
  echo "overview close snapshot remained after its fade: during=$during_blue after=$after_blue"
  exit 1
fi

echo "overview close card faded through the shared snapshot animation: blue $before_blue -> $during_blue -> $after_blue"
