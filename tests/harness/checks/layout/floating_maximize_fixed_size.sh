#!/usr/bin/env bash
# harness: outputs=1
# A fixed-size window opens floating, and a default_maximize rule matching it (an Electron splash shares its app's
# app_id) cannot grow it. It keeps its size, centered in the maximized box, instead of being pinned to the box's corner.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"

cat >> "$UMBRIEL_CONFIG" << 'EOF'

[animation]
duration_ms = 1000
curve = "linear"

[[window_rule]]
match.title = "^fixed-splash$"
default_focused = false
default_maximize = true
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" clock-freeze

FIXED_SIZE=1 HOLD_SIZE=1 "$CLIENT" fixed-splash 300 200 > "$UMBRIEL_RUNTIME_DIR/fixed-splash.log" 2>&1 &

field_of() {
  "$UMBRIEL" windows --json \
    | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

# Defaults: gap 8 and border 2 give an edge pad of 10, so the maximized box is 1260x700+10+10 on the 1280x720 output.
expected=300x200+490+260
actual=
for _ in $(seq 80); do
  actual="$(field_of fixed-splash w)x$(field_of fixed-splash h)+$(field_of fixed-splash x)+$(field_of fixed-splash y)"
  [[ $actual == "$expected" ]] && break
  sleep 0.1
done
first_configure=$(grep -m1 '^first-configure ' "$UMBRIEL_RUNTIME_DIR/fixed-splash.log" || true)
if [[ $first_configure != "first-configure 300 200 windowed" ]]; then
  echo "fixed-size splash received an oversized opening configure: ${first_configure:-missing}"
  exit 1
fi

readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/fixed-splash.png"
"$UMBRIEL" clock-advance 250
grim -o HEADLESS-1 "$SCREENSHOT"
read -r x y w h < <(
  "$UMBRIEL_PIXEL_PROBE" "$SCREENSHOT" bbox \
    '(r > 0.01 && g < 0.01 && b < 0.01) || (b > 0.01 && r < 0.01 && g < 0.01)'
)
if ((w <= 0 || h <= 0 || x < 490 || y < 260 || x + w > 790 || y + h > 460)); then
  echo "fixed-size splash escaped its final frame during opening: ${w}x${h}+$x+$y"
  exit 1
fi
"$UMBRIEL" clock-advance 1000
"$UMBRIEL" settle
actual="$(field_of fixed-splash w)x$(field_of fixed-splash h)+$(field_of fixed-splash x)+$(field_of fixed-splash y)"
if [[ $actual != "$expected" ]]; then
  echo "expected the fixed-size maximized float at $expected, got $actual"
  exit 1
fi

grim -o HEADLESS-1 "$SCREENSHOT"
# The child covers the parent with blue. Red outside it exposes a parent stretched past the reported geometry.
read -r red_pixels blue_pixels < <(
  "$UMBRIEL_PIXEL_PROBE" "$SCREENSHOT" count 'r > 0.9 && g < 0.1 && b < 0.1' \
    'b > 0.9 && r < 0.1 && g < 0.1' | xargs
)
if ((red_pixels != 0 || blue_pixels == 0)); then
  echo "fixed-size splash content escaped its frame or was missing: red=$red_pixels blue=$blue_pixels"
  exit 1
fi

echo "a fixed-size float opened by default_maximize kept its size, centered in the maximized box"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance.title_bar]
height = 28

[[workspace]]
index = 1
layout.gap = 12
layout.struts.left = 100
layout.struts.right = 40

[[window_rule]]
match.title = "^opening-maximized-"
default_floating = true
default_maximize = true
title_bar = "forced"

[[window_rule]]
match.title = "^opening-maximized-edges$"
default_maximize_to_edges = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

for kind in margin edges; do
  title=opening-maximized-$kind
  log=$UMBRIEL_RUNTIME_DIR/$title.log
  "$CLIENT" "$title" 640 480 > "$log" 2>&1 &
  for _ in $(seq 80); do
    grep -q '^mapped$' "$log" && break
    sleep 0.025
  done
  if [[ $kind == margin ]]; then
    configured="first-configure 1112 664 windowed"
  else
    configured="first-configure 1280 692 windowed"
  fi
  first_configure=$(grep -m1 '^first-configure ' "$log" || true)
  if [[ $first_configure != "$configured" ]]; then
    echo "$title: expected '$configured', got '${first_configure:-missing}'"
    exit 1
  fi
  "$UMBRIEL" clock-advance 1500
  "$UMBRIEL" settle
  actual="$(field_of "$title" w)x$(field_of "$title" h)"
  read -r _ width height _ <<< "$configured"
  if [[ $actual != "${width}x${height}" ]]; then
    echo "$title: opening configure was ${width}x${height}, final size was $actual"
    exit 1
  fi
done

echo "floating maximize configures accounted for workspace margins, title bars, and client size hints before map"
