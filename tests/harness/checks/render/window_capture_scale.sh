#!/usr/bin/env bash
# harness: outputs=2
# A persistent window capture must renegotiate physical buffer sizes after both
# output scale changes and moves, while preserving native client pixel detail.
# An unmapped auxiliary subsurface outside the window must never enlarge its capture.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BASE_CONFIG="$UMBRIEL_RUNTIME_DIR/window-scale-base.toml"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/window-scale-client.log"
readonly CAPTURE_LOG="$UMBRIEL_RUNTIME_DIR/window-scale-capture.log"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/window-scale.png"
cp "$UMBRIEL_CONFIG" "$BASE_CONFIG"

configure() {
  cat "$BASE_CONFIG" > "$UMBRIEL_CONFIG"
  cat >> "$UMBRIEL_CONFIG" <<CONFIG
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
[output.HEADLESS-1]
mode = "3200x2000"
scale = $1
position = [0, 0]
[output.HEADLESS-2]
scale = 1.25
position = [3200, 0]
[[window_rule]]
match.title = "^window-capture-scale$"
default_output = "HEADLESS-1"
default_floating = true
default_floating_size_px = { width = 322, height = 202 }
default_position = { x = 100, y = 100, anchor = "top_left" }
CONFIG
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle > /dev/null
}

configure 2.5
UNMAPPED_SUBSURFACE=1 "$UMBRIEL_FRACTIONAL_CLIENT" window-capture-scale 322 202 > "$CLIENT_LOG" 2>&1 &
client_pid=$!
trap 'kill -CONT "$client_pid" 2>/dev/null || true' EXIT
await_lines "$CLIENT_LOG" 'mapped .* scale 300/120' 1
"$UMBRIEL" settle > /dev/null
window=$("$UMBRIEL" windows --json | jq -ce '.[] | select(.title == "window-capture-scale")')
window_id=$(jq -r '.id' <<< "$window")
readonly WINDOW_LABEL="window:$window_id"

expect_capture() {
  local numerator=$1 denominator=$2 from_line=$3 description=$4
  local dimensions width height expected_width expected_height
  dimensions=$("$UMBRIEL" windows --json | jq -r --arg id "$window_id" '.[] | select(.id == $id) | "\(.w) \(.h)"')
  read -r width height <<< "$dimensions"
  expected_width=$(((width * numerator + denominator - 1) / denominator))
  expected_height=$(((height * numerator + denominator - 1) / denominator))
  for _ in $(seq 100); do
    if tail -n +"$from_line" "$CAPTURE_LOG" | awk -v label="$WINDOW_LABEL" -v w="$expected_width" -v h="$expected_height" '
      $1 == "constraints" && $2 == label && $3 == w && $4 == h { constraints = 1 }
      $1 == "frame" && $2 == label && $7 == w && $8 == h { frame = 1 }
      END { exit !(constraints && frame) }
    '; then
      break
    fi
    sleep 0.05
  done
  if ! tail -n +"$from_line" "$CAPTURE_LOG" | awk -v label="$WINDOW_LABEL" -v w="$expected_width" -v h="$expected_height" '
    $1 == "constraints" && $2 == label && $3 == w && $4 == h { constraints = 1 }
    $1 == "frame" && $2 == label && $7 == w && $8 == h { frame = 1 }
    END { exit !(constraints && frame) }
  '; then
    echo "$description: no new constraints and delivered frame at ${expected_width}x${expected_height}"
    cat "$CAPTURE_LOG"
    exit 1
  fi
}

grim -T "$window_id" "$IMAGE"
[[ $("$UMBRIEL_PIXEL_PROBE" "$IMAGE" size) == '805x505' ]] || {
  echo "initial fractional screenshot: expected 805x505, got $("$UMBRIEL_PIXEL_PROBE" "$IMAGE" size)"
  exit 1
}
# Native blue and green single-pixel columns must survive capture. Rendering at
# scale 1 and enlarging the result cannot produce this alternating pixel detail.
counts=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'b>0.99&&g<0.01&&r<0.01' 'g>0.99&&b<0.01&&r<0.01' '120x1+100+100')
[[ $counts == '60 60' ]] || { echo "native capture columns were resampled: $counts"; exit 1; }
[[ $("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 100 100) != "$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 101 100)" ]] || {
  echo 'adjacent native capture columns lost their alternating detail'
  exit 1
}
[[ $("$UMBRIEL" windows --json | jq -r --arg id "$window_id" '.[] | select(.id == $id) | "\(.w) \(.h)"') == '322 202' ]]

"$UMBRIEL_CAPTURE_CLIENT" --window "$window_id" > "$CAPTURE_LOG" 2>&1 &
capture_pid=$!
expect_capture 5 2 1 'initial fractional capture'

from_line=$(($(wc -l < "$CAPTURE_LOG") + 1))
# Freeze this contained client so no new surface commit can hide a missing
# capture-source refresh. Capturing its last committed buffer must still work.
kill -STOP "$client_pid"
configure 1
expect_capture 1 1 "$from_line" 'live transition to scale 1'
kill -CONT "$client_pid"

from_line=$(($(wc -l < "$CAPTURE_LOG") + 1))
configure 2.5
expect_capture 5 2 "$from_line" 'live transition back to scale 2.5'

from_line=$(($(wc -l < "$CAPTURE_LOG") + 1))
"$UMBRIEL" msg window-focus "$window_id" > /dev/null
"$UMBRIEL" msg window-move-to-workspace 1 --output HEADLESS-2 > /dev/null
"$UMBRIEL" settle > /dev/null
expect_capture 5 4 "$from_line" 'move to a scale 1.25 output with pixel rounding'

from_line=$(($(wc -l < "$CAPTURE_LOG") + 1))
"$UMBRIEL" msg window-move-to-workspace 1 --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
expect_capture 5 2 "$from_line" 'move back to scale 2.5'

from_line=$(($(wc -l < "$CAPTURE_LOG") + 1))
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
"$UMBRIEL" settle > /dev/null
expect_capture 5 2 "$from_line" 'fullscreen capture'
kill -TERM "$capture_pid"
wait "$capture_pid" || true
"$UMBRIEL" settle > /dev/null
grim -T "$window_id" "$IMAGE"
[[ $("$UMBRIEL_PIXEL_PROBE" "$IMAGE" size) == '3200x2000' ]]

echo 'window captures kept native pixels and updated the same session across scale changes, output moves, and fullscreen'
