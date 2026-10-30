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
  height=$(magick identify -format '%h' "$SCREENSHOT")
  magick "$SCREENSHOT" -alpha off -crop "1x1+1+$((height / 2))" +repage -depth 8 txt:- |
    sed -n 's/.*#\([0-9A-F]\{6\}\).*/\1/p' | head -1
}

# Compositor log lines lead with a timestamp, which await_lines' anchored match cannot skip.
await_log() {
  for _ in $(seq 60); do
    grep -q -- "$1" "$UMBRIEL_LOG" && return 0
    sleep 0.1
  done
  echo "timed out waiting for '$1' in the compositor log"
  exit 1
}

# grim is itself a capture, so a screenshot cannot show an output uncaptured; the log shows which outputs switched.
"$UMBRIEL_CAPTURE_CLIENT" --output HEADLESS-1 > "$CAPTURE_LOG" 2>&1 &
capture_pid=$!
await_lines "$CAPTURE_LOG" 'frame HEADLESS-1' 1
await_log "HEADLESS-1 screen effect '' -> 'edge'"
"$UMBRIEL" settle > /dev/null
if grep -q 'HEADLESS-2 screen effect' "$UMBRIEL_LOG"; then
  echo "capturing HEADLESS-1 switched HEADLESS-2's screen effect too"
  exit 1
fi
grim -o HEADLESS-1 "$SCREENSHOT"
got=$(edge_pixel)
if [[ $got != FF00FF ]]; then
  echo "during the cast: HEADLESS-1 edge reads #$got, expected the cast preset's #FF00FF"
  exit 1
fi

kill "$capture_pid"
wait "$capture_pid" 2> /dev/null || true
await_log "HEADLESS-1 screen effect 'edge' -> ''"

sed -i 's/^position = \[1280, 0\]$/position = [1280, 0]\nscreen_cast_effect = "off"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-2 "$SCREENSHOT"
got=$(edge_pixel)
if [[ $got != FFFFFF ]] || grep -q 'HEADLESS-2 screen effect' "$UMBRIEL_LOG"; then
  echo "screen_cast_effect = off still ran the cast preset on HEADLESS-2 (edge #$got)"
  exit 1
fi

echo "screen_cast runs only on the captured output, ends with the capture, and an output can opt out"
