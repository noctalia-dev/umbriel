#!/usr/bin/env bash
# harness: outputs=2
# Screen and cursor presets together: the screen effect inverts HEADLESS-1 only (HEADLESS-2 opts out with "off"), a
# time-reading screen program requests frames the same way a border or window one does, a static screen program keeps
# effect_frames flat and eligible at 0, the cursor square follows the pointer and stops requesting frames when hidden
# or off-output, the lock detaches both (eligible 0, frames flat), and grim sees them only with in_capture = true.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/effect-screen-cursor.png"
readonly OUTPUT_W=2560
readonly OUTPUT_H=720
cat > "$UMBRIEL_RUNTIME_DIR/invert.glsl" <<'GLSL'
vec4 screen(vec2 uv) { vec4 c = umbriel_sample(uv); return vec4(vec3(c.a) - c.rgb, c.a); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/invert-clock.glsl" <<'GLSL'
// Same inversion; the green term is invisible at 8 bits but keeps umbriel_time an active uniform, so the program
// requests frames like a time-reading border or window preset does.
vec4 screen(vec2 uv) { vec4 c = umbriel_sample(uv); return vec4(vec3(c.a) - c.rgb, c.a) + vec4(0.0, 0.001 * sin(umbriel_time), 0.0, 0.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/glow.glsl" <<'GLSL'
// The red term is invisible at 8 bits but keeps umbriel_time active, so the program requests frames.
vec4 cursor(vec2 uv) { return vec4(0.001 * sin(umbriel_time), 1.0, 0.0, 1.0); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
[colors]
backdrop = "#000000FF"
[input.cursor]
hide_timeout_ms = 0
[effects]
screen = "invert"
cursor = "glow"
in_capture = true
[effects.preset.invert]
kind = "screen"
shader = "invert.glsl"
[effects.preset.glow]
kind = "cursor"
shader = "glow.glsl"
radius = 40
[output."HEADLESS-1"]
position = [0, 0]
[output."HEADLESS-2"]
position = [1280, 0]
screen_effect = "off"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null

frames() { "$UMBRIEL" effect-frames --json | jq -r --arg n "$1" '.outputs[] | select(.name == $n) | .effect_frames'; }
eligible() { "$UMBRIEL" effect-frames --json | jq -r --arg n "$1" '.outputs[] | select(.name == $n) | .eligible'; }

# grim captures through screencopy, so every visual assertion runs with in_capture = true; the final section flips it
# to false and asserts the capture goes plain. Pointer at (300, 300) on HEADLESS-1.
"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 300 300 > /dev/null
"$UMBRIEL" settle > /dev/null
grim -s 1 -o HEADLESS-1 "$IMAGE"
# Inverted black backdrop is white away from the pointer, green within 40 px of it.
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 900 600)
if (( r < 240 || g < 240 || b < 240 )); then
  echo "the screen effect did not invert HEADLESS-1: $r $g $b"
  exit 1
fi
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 300 300)
if (( g < 240 || r > 20 )); then
  echo "the cursor effect did not paint around the pointer: $r $g $b"
  exit 1
fi
# Outside the 40 px radius the screen inversion still shows, undisturbed by the cursor square.
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 360 300)
if (( r < 240 || g < 240 || b < 240 )); then
  echo "the cursor square painted past its radius: $r $g $b"
  exit 1
fi
grim -s 1 -o HEADLESS-2 "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 900 600)
if (( r > 15 || g > 15 || b > 15 )); then
  echo "screen_effect = off did not disable the default on HEADLESS-2: $r $g $b"
  exit 1
fi
# The cursor square follows the pointer; the old spot is repainted with the plain inversion.
"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 700 200 > /dev/null
"$UMBRIEL" settle > /dev/null
grim -s 1 -o HEADLESS-1 "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 700 200)
(( g > 240 )) || { echo "the cursor square did not follow the pointer"; exit 1; }
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 300 300)
(( g < 250 || r > 200 )) || { echo "the old cursor square was not repainted"; exit 1; }

# Frames: a time-reading screen program requests frames the same way a time-reading cursor one does. Swap in the
# clock-reading screen shader alone (pointer parked off HEADLESS-1) to isolate the screen program's own eligibility.
"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 1500 300 > /dev/null
"$UMBRIEL" settle > /dev/null
sed -i 's/^shader = "invert.glsl"$/shader = "invert-clock.glsl"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
before=$(frames HEADLESS-1)
sleep 0.3 # real time: effect-only frames arrive on the output's own timer
(( $(frames HEADLESS-1) > before )) || { echo "a time-reading screen effect requested no frames"; exit 1; }
sed -i 's/^shader = "invert-clock.glsl"$/shader = "invert.glsl"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
# Back to the static screen program with the cursor still off HEADLESS-1: the ledger has nothing left to render, so
# effect_frames stays flat and eligible reports 0.
before=$(frames HEADLESS-1)
if (( $(eligible HEADLESS-1) != 0 )); then
  echo "a static screen program with no cursor on the output reported nonzero eligibility"
  exit 1
fi
sleep 0.3 # real time: a static screen program with no cursor on the output must request no frames
(( $(frames HEADLESS-1) == before )) || { echo "a static screen effect with no cursor on the output kept requesting frames"; exit 1; }

# The visible time-reading cursor on HEADLESS-1 requests frames; moving it to HEADLESS-2 stops them.
"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 300 300 > /dev/null
"$UMBRIEL" settle > /dev/null
before=$(frames HEADLESS-1)
sleep 0.3 # real time: effect-only frames arrive on the output's own timer
(( $(frames HEADLESS-1) > before )) || { echo "a visible cursor effect requested no frames"; exit 1; }
"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 1500 300 > /dev/null
"$UMBRIEL" settle > /dev/null
before=$(frames HEADLESS-1)
sleep 0.3 # real time: an off-output cursor instance must request no frames
(( $(frames HEADLESS-1) == before )) || { echo "an off-output cursor effect kept requesting frames on HEADLESS-1"; exit 1; }
# A hidden pointer stops frames too: back on HEADLESS-1, let the hide timeout elapse, then reloading with
# hide_timeout_ms = 0 un-hides the pointer immediately (updateHideTimer runs on the reload itself), restoring
# the square before the motion below is even sent.
"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 300 300 > /dev/null
sed -i 's/^hide_timeout_ms = 0$/hide_timeout_ms = 100/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
sleep 0.3 # real time: the pointer hides after hide_timeout_ms
"$UMBRIEL" settle > /dev/null
before=$(frames HEADLESS-1)
sleep 0.3 # real time: a hidden cursor instance must request no frames
(( $(frames HEADLESS-1) == before )) || { echo "a hidden cursor effect kept requesting frames"; exit 1; }
grim -s 1 -o HEADLESS-1 "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 300 300)
(( g < 250 || r > 200 )) || { echo "a hidden cursor still painted its square: $r $g $b"; exit 1; }
sed -i 's/^hide_timeout_ms = 100$/hide_timeout_ms = 0/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 300 300 > /dev/null
"$UMBRIEL" settle > /dev/null
grim -s 1 -o HEADLESS-1 "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 300 300)
(( g > 240 )) || { echo "hide_timeout_ms returning to 0 did not restore the cursor square: $r $g $b"; exit 1; }

# The lock detaches both effects (eligible drops to 0, frames stop growing); unlock restores them.
readonly LOCK_LOG="$UMBRIEL_RUNTIME_DIR/lock-client.log"
readonly LOCK_FIFO="$UMBRIEL_RUNTIME_DIR/lock-control"
mkfifo "$LOCK_FIFO"
exec {lock_fd}<> "$LOCK_FIFO"
"$UMBRIEL_LOCK_CLIENT" <&"$lock_fd" > "$LOCK_LOG" 2>&1 &
for _ in $(seq 100); do grep -q '^locked$' "$LOCK_LOG" && break; sleep 0.05; done
grep -q '^locked$' "$LOCK_LOG" || { echo "the session never locked"; exit 1; }
"$UMBRIEL" settle > /dev/null
grim -s 1 -o HEADLESS-1 "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 900 600)
# The lock client fills its surface with r=16 g=32 b=48; the inverted screen effect would read white.
(( r < 40 && g < 60 && b < 80 )) || { echo "the screen effect stayed attached under the session lock: $r $g $b"; exit 1; }
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 300 300)
(( g < 240 )) || { echo "the cursor effect stayed attached under the session lock: $r $g $b"; exit 1; }
locked_eligible=$(eligible HEADLESS-1)
(( locked_eligible == 0 )) || { echo "a session lock did not suspend the effect ledger: eligible=$locked_eligible"; exit 1; }
before=$(frames HEADLESS-1)
sleep 0.3 # real time: a locked session must produce no effect-only frames despite the lock client's own redraws
(( $(frames HEADLESS-1) == before )) || { echo "a session lock still produced effect-only frames"; exit 1; }
echo unlock >&"$lock_fd"
for _ in $(seq 100); do grep -q '^unlocked$' "$LOCK_LOG" && break; sleep 0.05; done
"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 300 300 > /dev/null
"$UMBRIEL" settle > /dev/null
grim -s 1 -o HEADLESS-1 "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 900 600)
(( r > 240 )) || { echo "the screen effect did not return after unlock: $r $g $b"; exit 1; }
unlocked_eligible=$(eligible HEADLESS-1)
(( unlocked_eligible > 0 )) || { echo "unlocking did not restore effect eligibility: eligible=$unlocked_eligible"; exit 1; }
before=$(frames HEADLESS-1)
sleep 0.3 # real time: effect-only frames arrive on the output's own timer
(( $(frames HEADLESS-1) > before )) || { echo "unlocking did not restart effect-only frames"; exit 1; }

# in_capture = false: grim sees the plain frame while the display keeps the effects (asserted through the frame
# counter continuing to run, since the display cannot be sampled without a capture).
sed -i 's/^in_capture = true$/in_capture = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
grim -s 1 -o HEADLESS-1 "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 900 600)
(( r < 15 && g < 15 )) || { echo "with in_capture = false grim still saw the screen effect: $r $g $b"; exit 1; }
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 300 300)
(( g < 15 )) || { echo "with in_capture = false grim still saw the cursor effect: $r $g $b"; exit 1; }
before=$(frames HEADLESS-1)
sleep 0.3 # real time: the display keeps requesting effect-only frames while captures go plain
(( $(frames HEADLESS-1) > before )) || { echo "with in_capture = false the display stopped requesting effect-only frames"; exit 1; }
echo "screen and cursor effects, the per-output override, radius bound, pointer tracking, frame gating, static eligibility, lock detachment, and capture policy verified"
