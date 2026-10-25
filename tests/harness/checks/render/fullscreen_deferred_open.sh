#!/usr/bin/env bash
# A tiled map followed by a fullscreen request in the same flush must reveal the opening buffer through the
# fullscreen path, since the view no longer participates in the arrange that admits ordinary tiled openers.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-deferred-open.log"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/fullscreen-deferred-open.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "scrolling"

[animation]
enabled = true

[animation.windows_in]
enabled = true
style = "fade"
duration_ms = 100
curve = "linear"

[colors]
backdrop = "#000000FF"
EOF
"$UMBRIEL" msg config-reload > /dev/null

# The helper queues its first buffered mapping commit, set_fullscreen, and the next commit in one client flush.
REQUEST_FULLSCREEN=1 FOLLOW_CONFIGURES=1 LOG_CONFIGURES=1 FILL_COLOR=0xFF0000FF \
  "$UMBRIEL_UNMAP_CLIENT" fullscreen-deferred-open 640 480 > "$CLIENT_LOG" 2>&1 &
await_lines "$CLIENT_LOG" 'mapped$' 1
await_lines "$CLIENT_LOG" 'configured-state=1280x720 fullscreen$' 1

first_configure=$(grep -m1 '^configured-state=' "$CLIENT_LOG")
if [[ $first_configure != *' windowed' ]]; then
  echo "the opener did not begin with a windowed configure: $first_configure"
  exit 1
fi
"$UMBRIEL" settle > /dev/null

window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "fullscreen-deferred-open")')
if ! jq -e '.active and .fullscreen and .w == 1280 and .h == 720' \
  <<< "$window" > /dev/null; then
  echo "the opening fullscreen request did not become active at the output size: $window"
  exit 1
fi

grim "$IMAGE"
for point in '16 16' '640 360' '1264 704'; do
  read -r x y <<< "$point"
  read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
  if ((red > 20 || green > 20 || blue < 230)); then
    echo "the deferred fullscreen opener was not rendered at $x,$y: $red $green $blue"
    exit 1
  fi
done

echo "a same-batch tiled map and fullscreen request reveals the fullscreen buffer"
