#!/usr/bin/env bash
# The default_maximize_to_edges window rule must expand a matching window to the
# usable-area edges at map time. A window with the same layout width fraction but
# no matching rule stays at that fraction, so the widening is attributable to the
# rule rather than to any unrelated full-width fallback. A solid client on the
# black headless output makes the presented width measurable.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly BASELINE="$UMBRIEL_RUNTIME_DIR/initial-maximize-to-edges-baseline.png"
readonly RULED="$UMBRIEL_RUNTIME_DIR/initial-maximize-to-edges-ruled.png"
readonly MAXIMIZED_LOG="$UMBRIEL_RUNTIME_DIR/initial-maximize-ruled.log"
readonly EDGES_LOG="$UMBRIEL_RUNTIME_DIR/initial-maximize-to-edges-ruled.log"

presented_width() {
  magick "$1" -alpha off -crop 1280x1+0+360 +repage -fuzz 2% \
    -fill black +opaque '#5577AA' -fill white -opaque '#5577AA' -colorspace gray \
    -format '%[fx:round(w*mean)]' info:
}
assert_stable_initial_size() {
  local log=$1 label=$2 first last
  first=$(grep '^configured-size=' "$log" | sed -n '1p')
  last=$(grep '^configured-size=' "$log" | sed -n '$p')
  if [[ -z $first || $first != "$last" ]]; then
    echo "$label opened with a different size than its final configure: first=${first:-missing} last=${last:-missing}"
    return 1
  fi
}

spawn_client() {
  local title=$1 log=$2
  env LOG_CONFIGURES=1 "$CLIENT" "$title" 1280 720 > "$log" 2>&1 &
  CLIENT_PID=$!
  for _ in $(seq 60); do
    grep -q '^mapped$' "$log" && return 0
    sleep 0.05
  done
  echo "$title client never mapped: $(cat "$log")"
  return 1
}

stop_client() {
  kill -KILL "$CLIENT_PID" 2>/dev/null || true
  wait "$CLIENT_PID" 2>/dev/null || true
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[layout.scrolling]
default_width_fraction = 0.5

[[window_rule]]
match.title = "^maximize-ruled$"
default_maximize = true

[[window_rule]]
match.title = "^edges-ruled$"
default_maximize_to_edges = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

# The unmatched window keeps the half-width fraction: the measurement baseline.
spawn_client edges-baseline "$UMBRIEL_RUNTIME_DIR/initial-maximize-to-edges-baseline.log" || exit 1
sleep 0.3
grim "$BASELINE"
baseline_width=$(presented_width "$BASELINE")
stop_client
if (( baseline_width > 900 )); then
  echo "baseline window was not laid out at the half-width fraction: width=$baseline_width"
  exit 1
fi
readonly MAXIMIZED_SCREENSHOT="$UMBRIEL_RUNTIME_DIR/initial-maximize-ruled.png"
spawn_client maximize-ruled "$MAXIMIZED_LOG" || exit 1
sleep 0.3
grim "$MAXIMIZED_SCREENSHOT"
maximized_width=$(presented_width "$MAXIMIZED_SCREENSHOT")
stop_client
assert_stable_initial_size "$MAXIMIZED_LOG" "default_maximize"
if (( maximized_width < 1100 || maximized_width - baseline_width < 400 )); then
  echo "default_maximize did not expand the window to full column width: baseline=$baseline_width maximized=$maximized_width"
  exit 1
fi

# Same fraction, but the rule maximizes it to the edges at map.
spawn_client edges-ruled "$EDGES_LOG" || exit 1
sleep 0.3
grim "$RULED"
ruled_width=$(presented_width "$RULED")
stop_client
assert_stable_initial_size "$EDGES_LOG" "default_maximize_to_edges"

if (( ruled_width < 1100 || ruled_width - baseline_width < 400 )); then
  echo "default_maximize_to_edges did not expand the window to the edges at map: baseline=$baseline_width ruled=$ruled_width"
  exit 1
fi

echo "maximize rules used one initial size and expanded mapped windows: $baseline_width -> $maximized_width -> $ruled_width"
