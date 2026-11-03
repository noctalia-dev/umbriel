#!/usr/bin/env bash
# harness: outputs=2
# `effects.screen_cast` replaces the screen effect on an output only while a client captures that output whole: the
# other output keeps its own, the capture ending restores it, and `screen_cast_effect = "off"` opts an output out.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/effect-screen-cast.png"
readonly CAPTURE_LOG="$UMBRIEL_RUNTIME_DIR/effect-screen-cast-capture.log"

cat > "$UMBRIEL_RUNTIME_DIR/edge.glsl" <<'GLSL'
vec4 screen(vec2 uv) {
  vec2 px = uv * umbriel_size;
  vec2 e = min(px, umbriel_size - px);
  return min(e.x, e.y) < 4.0 ? vec4(1.0, 0.0, 1.0, 1.0) : umbriel_sample(uv);
}
GLSL
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false
[colors]
backdrop = "#FFFFFFFF"
[effects]
screen_cast = "edge"
in_capture = true
[effects.preset.edge]
kind = "screen"
shader = "edge.glsl"
[output."HEADLESS-1"]
position = [0, 0]
[output."HEADLESS-2"]
position = [1280, 0]
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

edge_pixel() {
  local height
  IFS=x read -r _ height < <("$UMBRIEL_PIXEL_PROBE" "$SCREENSHOT" size)
  "$UMBRIEL_PIXEL_PROBE" "$SCREENSHOT" pixel 1 "$((height / 2))"
}

screen_effect() {
  "$UMBRIEL" effects --json | jq -r --arg name "$1" '.owners[] | select(.type == "output" and .name == $name) | .slots.screen.name'
}

await_screen_effect() {
  for _ in $(seq 60); do
    [[ $(screen_effect "$1") == "$2" ]] && return 0
    sleep 0.1
  done
  echo "timed out waiting for $1's screen effect to become '$2', still '$(screen_effect "$1")'"
  exit 1
}

# grim is itself a capture, so a screenshot cannot show an output uncaptured; IPC shows which outputs switched.
"$UMBRIEL_CAPTURE_CLIENT" --output HEADLESS-1 > "$CAPTURE_LOG" 2>&1 &
capture_pid=$!
await_lines "$CAPTURE_LOG" 'frame HEADLESS-1' 1
await_screen_effect HEADLESS-1 edge
"$UMBRIEL" settle > /dev/null
if [[ -n $(screen_effect HEADLESS-2) ]]; then
  echo "capturing HEADLESS-1 switched HEADLESS-2's screen effect too"
  exit 1
fi
grim -o HEADLESS-1 "$SCREENSHOT"
read -r red green blue < <(edge_pixel)
if ((red != 255 || green != 0 || blue != 255)); then
  echo "during the cast: HEADLESS-1 edge reads $red $green $blue, expected the cast preset's 255 0 255"
  exit 1
fi

kill "$capture_pid"
wait "$capture_pid" 2> /dev/null || true
await_screen_effect HEADLESS-1 ''

sed -i 's/^position = \[1280, 0\]$/position = [1280, 0]\nscreen_cast_effect = "off"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL_CAPTURE_CLIENT" --output HEADLESS-2 > "$CAPTURE_LOG" 2>&1 &
capture_pid=$!
await_lines "$CAPTURE_LOG" 'frame HEADLESS-2' 1
"$UMBRIEL" settle > /dev/null
cast_effect=$(screen_effect HEADLESS-2)
grim -o HEADLESS-2 "$SCREENSHOT"
read -r red green blue < <(edge_pixel)
kill "$capture_pid"
wait "$capture_pid" 2> /dev/null || true
if [[ -n $cast_effect ]] || ((red != 255 || green != 255 || blue != 255)); then
  echo "screen_cast_effect = off still ran '$cast_effect' on HEADLESS-2 (edge $red $green $blue)"
  exit 1
fi

echo "screen_cast runs only on the captured output, ends with the capture, and an output can opt out"
