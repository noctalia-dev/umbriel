#!/usr/bin/env bash
# Focus-dependent opacity must survive independent commits from a desynchronized
# child surface. Firefox presents its visible content through exactly this path.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/subsurface-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/subsurface-opacity.log"
readonly FOCUS_LOG="$UMBRIEL_RUNTIME_DIR/subsurface-focus-target.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/rule-opacity-subsurface-commit.png"

if [[ ! -x $CLIENT ]]; then
  echo "subsurface client not built at $CLIENT"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
animation_ms = 1
border_width = 0
corner_radius = 0
backdrop_color = "#00FF00FF"

[[window_rule]]
match.app_id = "^subsurface-opacity$"
match.is_focused = true
opacity = 0.75

[[window_rule]]
match.app_id = "^subsurface-opacity$"
match.is_focused = false
opacity = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" "subsurface-opacity" 640 480 animate > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "subsurface client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

# Mapping a second window moves focus away and selects the 0.5 rule. Let the
# Firefox-like child redraw independently before measuring its visible content.
"$CLIENT" "subsurface-focus-target" 640 480 > "$FOCUS_LOG" 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 2 ]] && break
  sleep 0.05
done
windows=$("$UMBRIEL" windows --json)
if [[ $(jq 'length' <<< "$windows") -ne 2 ]]; then
  echo "timed out waiting for both windows: $windows"
  exit 1
fi
if [[ $(jq -r '.[] | select(.title == "subsurface-opacity") | .focused' <<< "$windows") != false ]]; then
  echo "subsurface window did not lose focus: $windows"
  exit 1
fi

sleep 0.5
windows=$("$UMBRIEL" windows --json)
read -r win_x win_y win_w win_h <<< "$(
  jq -r '.[] | select(.title == "subsurface-opacity") | "\(.x) \(.y) \(.w) \(.h)"' <<< "$windows"
)"
grim "$SCREENSHOT"

# Both the red parent and blue child receive the 0.5 rule opacity. Their stacked
# blend over green is approximately red 64, green 64, blue 128. An opaque child
# after a missed commit restore is instead blue 255 with red and green near zero.
read -r red green blue <<< "$(
  magick "$SCREENSHOT" -crop "40x40+$((win_x + win_w / 2 - 20))+$((win_y + win_h / 2 - 20))" \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
)"
if (( red < 35 || red > 100 || green < 35 || green > 100 || blue < 90 || blue > 170 )); then
  echo "focus rule opacity was lost after a child commit: red=$red green=$green blue=$blue"
  exit 1
fi

echo "focus rule opacity survived independent subsurface commits: red=$red green=$green blue=$blue"
