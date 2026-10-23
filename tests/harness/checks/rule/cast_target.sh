#!/usr/bin/env bash
# `match.is_cast_target` marks the window someone is watching: it holds while an
# image-copy session reads the window's capture source and lets go when the
# session ends. Two identical floats share a red frame; only the one a capture
# client casts may turn green, and it must turn back once the cast stops.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/rule-cast-target.png"
readonly CAPTURE_LOG="$UMBRIEL_RUNTIME_DIR/rule-cast-target-capture.log"
readonly CLOSE_CAPTURE_LOG="$UMBRIEL_RUNTIME_DIR/rule-cast-target-close-capture.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[colors]
backdrop = "#FFFFFFFF"

[colors.border]
focused = "#FF0000FF"
unfocused = "#FF0000FF"

[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[[window_rule]]
match.app_id = "^cast-a$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 80, y = 120, anchor = "top_left" }

[[window_rule]]
match.app_id = "^cast-b$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 620, y = 120, anchor = "top_left" }

[[window_rule]]
match.is_cast_target = true
border_color_focused = "#00FF00FF"
border_color_unfocused = "#00FF00FF"
EOF
"$UMBRIEL" msg config-reload > /dev/null

window_of() {
  "$UMBRIEL" windows --json | jq -c --arg a "$1" '.[] | select(.app_id == $a)'
}

wait_mapped() {
  for _ in $(seq 80); do
    [[ -n $(window_of "$1") ]] && return 0
    sleep 0.1
  done
  echo "window '$1' never mapped"
  exit 1
}

start_client() {
  foot --config=/dev/null --app-id="$1" --override=colors.background=000000 sh -c 'sleep 120' \
    > /dev/null 2>&1 &
  client_pid=$!
  wait_mapped "$1"
}

# The frame pixel two pixels left of the window's content, at mid height, as RRGGBB.
frame_of() {
  local window x y
  window=$(window_of "$1")
  x=$(($(jq -r '.x' <<< "$window") - 2))
  y=$(($(jq -r '.y' <<< "$window") + 150))
  magick "$SCREENSHOT" -alpha off -crop "1x1+$x+$y" +repage -depth 8 txt:- |
    sed -n 's/.*#\([0-9A-F]\{6\}\).*/\1/p' | head -1
}

expect_frames() {
  local label=$1 want_a=$2 want_b=$3 a b
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$SCREENSHOT"
  a=$(frame_of cast-a)
  b=$(frame_of cast-b)
  if [[ $a != "$want_a" || $b != "$want_b" ]]; then
    echo "$label: frames read cast-a #$a cast-b #$b, expected #$want_a #$want_b"
    exit 1
  fi
}

start_client cast-a
start_client cast-b
expect_frames "before the cast" FF0000 FF0000

"$UMBRIEL_CAPTURE_CLIENT" --window "$(window_of cast-a | jq -r '.id')" > "$CAPTURE_LOG" 2>&1 &
capture_pid=$!
await_lines "$CAPTURE_LOG" 'frame window:' 1
expect_frames "during the cast" 00FF00 FF0000

kill "$capture_pid"
wait "$capture_pid" 2> /dev/null || true
expect_frames "after the cast" FF0000 FF0000

# Closing a cast window ends its session inside ~View. A third window keeps the dying
# view mid-registry, where the erase leaves its slot null: the live crash.
start_client cast-c
cast_c_pid=$client_pid
cast_c_id=$(window_of cast-c | jq -r '.id')
"$UMBRIEL_CAPTURE_CLIENT" --window "$cast_c_id" > "$CLOSE_CAPTURE_LOG" 2>&1 &
capture_pid=$!
await_lines "$CLOSE_CAPTURE_LOG" 'frame window:' 1
kill "$cast_c_pid"
closed=false
for _ in $(seq 80); do
  [[ -z $(window_of cast-c) ]] && closed=true && break
  sleep 0.1
done
kill "$capture_pid" 2> /dev/null || true
wait "$capture_pid" 2> /dev/null || true
if ! "$UMBRIEL" settle > /dev/null 2>&1; then
  echo "compositor died closing a window under a cast"
  exit 1
fi
if [[ $closed != true ]]; then
  echo "cast-c never closed"
  exit 1
fi

echo "is_cast_target follows the window cast: red -> green -> red, the other window stays red; closing a cast window holds"
