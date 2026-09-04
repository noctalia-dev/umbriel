#!/usr/bin/env bash
# Opening overview dismisses a visible scratchpad immediately instead of showing its ordinary hide fade underneath.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/overview-scratchpad-transition-client.log"
readonly BEFORE="$UMBRIEL_RUNTIME_DIR/overview-scratchpad-transition-before.png"
readonly EARLY="$UMBRIEL_RUNTIME_DIR/overview-scratchpad-transition-early.png"
readonly SETTLED="$UMBRIEL_RUNTIME_DIR/overview-scratchpad-transition-settled.png"

sample_rgb() {
  local image=$1 x=$2 y=$3
  magick "$image" -crop "20x20+$((x - 10))+$((y - 10))" -colorspace RGB \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

assert_blue() {
  local label=$1 image=$2 red green blue
  read -r red green blue <<< "$(sample_rgb "$image" 640 360)"
  if ((blue < 100 || blue < red + 40)); then
    echo "$label scratchpad window is not visible: red=$red green=$green blue=$blue"
    return 1
  fi
}

assert_white() {
  local label=$1 image=$2 x=$3 y=$4 red green blue
  read -r red green blue <<< "$(sample_rgb "$image" "$x" "$y")"
  if ((red < 240 || green < 240 || blue < 240)); then
    echo "$label retained the scratchpad transition: red=$red green=$green blue=$blue"
    return 1
  fi
}

wait_for_empty_workspace() {
  local workspace=
  for _ in $(seq 60); do
    workspace=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "overview-scratchpad-transition") | .workspace')
    [[ -z $workspace ]] && return 0
    sleep 0.1
  done
  echo "scratchpad window retained workspace '$workspace': $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.windows_in]
enabled = false

[animation.overview]
enabled = false

[animation.scratchpad]
enabled = true
duration_ms = 5000
curve = "linear"
dim = 0.5
blur = false
scale = 0.0

[colors]
backdrop = "#FFFFFFFF"

[colors.overview]
background_tint = "#00000000"
workspace_background = "#00000000"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.blur]
enabled = false

[overview]
background_blur = false
shortcuts = false

[[window_rule]]
match.title = "^overview-scratchpad-transition$"
default_floating = true
default_size = [420, 260]
default_position = { x = 430, y = 230, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" overview-scratchpad-transition 420 260 > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "scratchpad transition client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

"$UMBRIEL" msg window-move-to-scratchpad:HEADLESS-1 > /dev/null
wait_for_empty_workspace
"$UMBRIEL" msg scratchpad-toggle:HEADLESS-1 > /dev/null
sleep 0.1
grim "$BEFORE"
assert_blue before-overview "$BEFORE"

sed -i '0,/^enabled = false$/s//enabled = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.1
grim "$EARLY"

sed -i '0,/^enabled = true$/s//enabled = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
grim "$SETTLED"
assert_white settled-window "$SETTLED" 640 360
assert_white settled-backdrop "$SETTLED" 40 40
assert_white overview-window "$EARLY" 640 360
assert_white overview-backdrop "$EARLY" 40 40

echo "overview entry dismissed the scratchpad window and backdrop without exposing their hide fade"
