#!/usr/bin/env bash
# Border, window, cursor, and animation presets read the external audio level and availability like the screen preset
# in effect/audio: each kind's pixels follow the level a producer sends and report the feed as available.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/effect-audio-kinds.png"
readonly LEVEL="$UMBRIEL_RUNTIME_DIR/audio-level"
readonly BASE="$UMBRIEL_RUNTIME_DIR/effect-audio-kinds-base.toml"
cp "$UMBRIEL_CONFIG" "$BASE"

# Red is the level and green the availability, so a lit green channel also proves the feed reached the shader.
for kind in border window cursor animation; do
  cat > "$UMBRIEL_RUNTIME_DIR/$kind.glsl" <<GLSL
vec4 $kind(vec2 uv) { return vec4(umbriel_audio_level(), umbriel_audio_available(), 0.0, 1.0); }
GLSL
done

# Keeps one producer connection fresh, sending whatever level the file holds.
cat > "$UMBRIEL_RUNTIME_DIR/audio-feed.py" <<'PY'
import json
import os
import socket
import time

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.connect(os.environ['UMBRIEL_SOCKET'])
while True:
    with open(os.environ['AUDIO_LEVEL_FILE']) as level:
        value = float(level.read())
    sock.sendall((json.dumps({'cmd': 'effect-audio', 'version': 1, 'level': value}) + '\n').encode())
    if b'"ok":true' not in sock.recv(256):
        raise SystemExit('producer refused')
    time.sleep(0.02)
PY

set_level() {
  printf '%s' "$1" > "$LEVEL.tmp"
  mv "$LEVEL.tmp" "$LEVEL"
}
set_level 1
AUDIO_LEVEL_FILE="$LEVEL" python3 "$UMBRIEL_RUNTIME_DIR/audio-feed.py" > "$UMBRIEL_RUNTIME_DIR/audio-feed.log" 2>&1 &

# A fresh config per kind: $1 is the border width, $2 whether animations run, $3 the [effects] selection.
reset_config() {
  cat "$BASE" > "$UMBRIEL_CONFIG"
  cat >> "$UMBRIEL_CONFIG" <<EOF

[colors]
backdrop = "#000000FF"
[animation]
enabled = $2
[appearance]
border_width = $1
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[input.cursor]
hide_timeout_ms = 0
[effects]
in_capture = true
$3
[[window_rule]]
match.title = "^audio-kind-"
default_floating = true
default_position = { x = 200, y = 200, anchor = "top_left" }
EOF
}
# The preset every kind selects: $1 is its kind, $2 any extra key.
preset() {
  printf '[effects.preset.audio]\nkind = "%s"\nshader = "%s.glsl"\n%s\n' "$1" "$1" "${2:-}" >> "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

spawn() {
  FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" "$1" 300 200 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]]
  read -r x y w h < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
}

# Waits for the pixel at $1,$2 to show red within 6 of $3 with green lit, or fails naming $4.
await_audio() {
  local r g b
  for _ in $(seq 80); do
    grim "$IMAGE"
    read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$1" "$2")
    if (( r >= $3 - 6 && r <= $3 + 6 && g > 250 )); then
      return 0
    fi
    sleep 0.05
  done
  echo "$4: expected red near $3 with green lit, got $r $g $b"
  exit 1
}

# Two levels per kind, so a constant colour cannot pass.
assert_follows() {
  set_level 1
  await_audio "$1" "$2" 255 "$3 at level 1"
  set_level 0.25
  await_audio "$1" "$2" 64 "$3 at level 0.25"
}

### Border: the padding above the focused window's ring.
reset_config 4 false 'border = "audio"'
preset border 'padding = 20'
spawn audio-kind-border
"$UMBRIEL" settle > /dev/null
assert_follows "$((x + w / 2))" "$((y - 12))" "the border preset"
"$UMBRIEL" msg window-close > /dev/null

### Window: the middle of the window.
reset_config 0 false 'window = "audio"'
preset window
spawn audio-kind-window
"$UMBRIEL" settle > /dev/null
assert_follows "$((x + w / 2))" "$((y + h / 2))" "the window preset"
"$UMBRIEL" msg window-close > /dev/null

### Cursor: the pointer's own pixel.
reset_config 0 false 'cursor = "audio"'
preset cursor 'radius = 40'
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 300 300 > /dev/null
"$UMBRIEL" settle > /dev/null
assert_follows 300 300 "the cursor preset"

### Animation: an opening window held at the start of a long animation.
reset_config 0 true ''
cat >> "$UMBRIEL_CONFIG" <<'EOF'
[animation.windows_in]
style = "none"
effect = "audio"
duration_ms = 10000
curve = "linear"
EOF
preset animation
"$UMBRIEL" clock-freeze
spawn audio-kind-animation
assert_follows "$((x + w / 2))" "$((y + h / 2))" "the animation preset"
