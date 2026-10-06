#!/usr/bin/env bash
# Four no-time effect kinds share one explicit external synthetic input. Tests
# pixels, frozen-clock injection, zero-input negative control, quiet unchanged
# input, time-disabled borders, source sharing and last-demand shutdown.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/audio.png"
readonly HELPER="$(realpath "$(dirname "$UMBRIEL_UNMAP_CLIENT")/audio-synthetic")"
cat > "$UMBRIEL_RUNTIME_DIR/audio-screen.glsl" <<'GLSL'
vec4 screen(vec2 uv) {
  vec4 c = umbriel_sample(uv);
  return vec4(c.r + 0.2 * umbriel_audio_rms() * c.a, c.g, c.b, c.a);
}
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/audio-window.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(0.0, umbriel_audio_rms(), 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/audio-border.glsl" <<'GLSL'
vec4 border(vec2 uv) { return vec4(0.0, 0.0, umbriel_audio_level(), 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/audio-cursor.glsl" <<'GLSL'
vec4 cursor(vec2 uv) { return vec4(vec3(umbriel_audio_band(0.5)), 1.0); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
enabled = false
[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
[input.cursor]
hide_timeout_ms = 0
[effects]
border = "audio-border"
window = "audio-window"
screen = "audio-screen"
cursor = "audio-cursor"
in_capture = true
max_fps = 20
[effects.audio.sources.desktop]
provider = "external"
mode = "playback"
target = "explicit-test-source"
executable = "$HELPER"
args = ["--external-test", "--silence"]
[effects.preset.audio-screen]
kind = "screen"
shader = "audio-screen.glsl"
audio = "desktop"
[effects.preset.audio-window]
kind = "window"
shader = "audio-window.glsl"
audio = "desktop"
[effects.preset.audio-border]
kind = "border"
shader = "audio-border.glsl"
audio = "desktop"
animated = false
speed = 0
padding = 20
[effects.preset.audio-cursor]
kind = "cursor"
shader = "audio-cursor.glsl"
audio = "desktop"
radius = 30
[[window_rule]]
match.title = "^audio-fixture$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFF000000 "$UMBRIEL_UNMAP_CLIENT" audio-fixture 300 200 > "$UMBRIEL_RUNTIME_DIR/audio-client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "audio-fixture")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
read -r x y w h < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 800 300 > /dev/null
"$UMBRIEL" settle > /dev/null
for _ in $(seq 100); do
  "$UMBRIEL" effects --json | jq -e '.audio[] | select(.name == "desktop") | .available' > /dev/null && break
  sleep 0.02
done
"$UMBRIEL" effects --json | jq -e '.audio_demanded_sources == 1 and (.audio[] | select(.name == "desktop") | .available)' > /dev/null

inject() {
  "$UMBRIEL" audio-inject "$(jq -nc --argjson v "$1" '{source:"desktop",rms:$v,peak:$v,envelope:$v,bands:[range(16)|$v]}')" > /dev/null
  "$UMBRIEL" settle > /dev/null
}
frames() { "$UMBRIEL" effect-frames --json | jq -er '.outputs[0].effect_frames'; }
pixel() { "$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$1" "$2"; }

"$UMBRIEL" clock-freeze > /dev/null
inject 0.8
grim -s 1 -o HEADLESS-1 "$IMAGE"
read -r r g b < <(pixel 1100 600)
(( r > 30 && g < 20 && b < 20 )) || { echo "no-time screen audio missing: $r $g $b"; exit 1; }
read -r r g b < <(pixel "$((x+w/2))" "$((y+h/2))")
(( g > 150 && b < 20 )) || { echo "no-time window audio missing: $r $g $b"; exit 1; }
read -r r g b < <(pixel "$((x+w/2))" "$((y-12))")
(( b > 150 && g < 20 )) || { echo "animated=false/speed=0 border audio missing: $r $g $b"; exit 1; }
read -r r g b < <(pixel 810 310)
(( r > 150 && g > 150 && b > 150 )) || { echo "no-time cursor band input missing: $r $g $b"; exit 1; }

# Explicit fixture input changes while frozen; holding it at zero must remove
# every response without unbinding shaders or moving the animation clock.
inject 0
grim -s 1 -o HEADLESS-1 "$IMAGE"
for point in "1100 600" "$((x+w/2)) $((y+h/2))" "$((x+w/2)) $((y-12))" "810 310"; do
  read -r px py <<< "$point"
  read -r r g b < <(pixel "$px" "$py")
  (( r < 20 && g < 20 && b < 20 )) || { echo "held-zero audio control failed at $point: $r $g $b"; exit 1; }
done
"$UMBRIEL" clock-resume > /dev/null
"$UMBRIEL" settle > /dev/null
before=$(frames)
sleep 0.3 # real time: unchanged provider silence must not schedule effect-only frames
(( $(frames) == before )) || { echo "unchanged audio kept requesting effect-only frames"; exit 1; }

# Source declarations and compiled programs do not keep acquisition alive after
# the last usable consumer is removed.
sed -i '/^\[effects\]$/,/^\[effects.audio/ s/^\(border\|window\|screen\|cursor\) = .*/\1 = ""/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" effects --json | jq -e '.audio_demanded_sources == 0' > /dev/null
echo "shared no-time audio changed screen/window/border/cursor pixels, frozen injection and zero control passed, silence settled and last demand stopped"
