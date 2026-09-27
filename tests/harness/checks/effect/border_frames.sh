#!/usr/bin/env bash
# Effect-only frames follow the animation clock, not real time: a frozen clock renders the same instant across
# frames and through a close snapshot, effects.max_fps caps the render rate to its own due-frame interval, and effect
# time keeps advancing while another animation drives the output's frames.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/effect-border-frames.png"
cat > "$UMBRIEL_RUNTIME_DIR/clock.glsl" <<'GLSL'
// The red channel encodes the current effect time, so a frozen clock renders an unchanging ring and an advancing
// clock renders a different one. The 0.3 factor keeps whole-second steps (max_fps = 1) from wrapping to the same value.
vec4 border(vec2 uv) { return vec4(fract(umbriel_time * 0.3), 0.0, 0.0, 1.0); }
GLSL

readonly BASE="$UMBRIEL_RUNTIME_DIR/frames-base.toml"
cp "$UMBRIEL_CONFIG" "$BASE"

effect_field() {
  "$UMBRIEL" effect-frames --json | jq -er ".outputs[0].$1" || {
    echo "effect-frames reported no .outputs[0].$1" >&2
    return 1
  }
}
frames() { effect_field effect_frames; }
eligible() { effect_field eligible; }
ring_pixel() { "$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$1" "$2"; }

open_window() {
  local title=${1:-frame-one} fill=${2:-0xFF00FF00}
  FILL_COLOR=$fill "$UMBRIEL_UNMAP_CLIENT" "$title" 300 200 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]]
  "$UMBRIEL" settle > /dev/null
  read -r x y w h id < <(jq -r '"\(.x) \(.y) \(.w) \(.h) \(.id)"' <<< "$window")
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

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
[effects]
border = "clock"
[effects.preset.clock]
kind = "border"
shader = "clock.glsl"
padding = 20
[effects.preset.clock.light]
spread = 40
intensity = 4
threshold = 0.2
[[window_rule]]
match.title = "^frame-one$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
open_window
ring_x=$((x + w / 2))
ring_y=$((y - 12))
light_x=$((x + w / 2))
light_y=$((y - 50))

# 1. A frozen clock renders the same instant across frames; only clock-advance moves it.
"$UMBRIEL" clock-freeze
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r ring_r1 ring_g1 ring_b1 < <(ring_pixel "$ring_x" "$ring_y")
read -r light_r1 light_g1 light_b1 < <(ring_pixel "$light_x" "$light_y")
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r ring_r2 ring_g2 ring_b2 < <(ring_pixel "$ring_x" "$ring_y")
read -r light_r2 light_g2 light_b2 < <(ring_pixel "$light_x" "$light_y")
if [[ "$ring_r1 $ring_g1 $ring_b1" != "$ring_r2 $ring_g2 $ring_b2" ]]; then
  echo "a frozen clock's ring pixel changed between two frames: $ring_r1 $ring_g1 $ring_b1 -> $ring_r2 $ring_g2 $ring_b2"
  exit 1
fi
if [[ "$light_r1 $light_g1 $light_b1" != "$light_r2 $light_g2 $light_b2" ]]; then
  echo "a frozen clock's light pixel changed between two frames: $light_r1 $light_g1 $light_b1 -> $light_r2 $light_g2 $light_b2"
  exit 1
fi
"$UMBRIEL" clock-advance 500
grim "$IMAGE"
read -r ring_r3 ring_g3 ring_b3 < <(ring_pixel "$ring_x" "$ring_y")
if [[ "$ring_r1 $ring_g1 $ring_b1" == "$ring_r3 $ring_g3 $ring_b3" ]]; then
  echo "advancing the clock 500ms did not change the time-keyed ring pixel: stayed at $ring_r1 $ring_g1 $ring_b1"
  exit 1
fi

# 2. A close snapshot under a frozen clock keeps rendering that frozen instant, and the frozen clock still produces
# no effect-only frames while the compositor stays up.
# The reconfigure below flips [animation] from off to on, which can start the border's own focus-color transition;
# resuming the clock first lets that transition (and the reload itself) settle before this check freezes it again.
"$UMBRIEL" clock-resume
cat "$BASE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = true
[animation.windows_in]
enabled = false
[animation.windows_move]
enabled = false
[animation.windows_out]
enabled = true
style = "fade"
duration_ms = 5000
curve = "linear"
[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
[effects]
border = "clock"
[effects.preset.clock]
kind = "border"
shader = "clock.glsl"
padding = 20
[[window_rule]]
match.title = "^frame-one$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" clock-freeze
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r ring_r5 ring_g5 ring_b5 < <(ring_pixel "$ring_x" "$ring_y")
before_close=$(frames)
"$UMBRIEL" msg "window-close:$id" > /dev/null
# The close animation is now running but frozen; settle would refuse (an active animation never finishes on a
# frozen clock), so this polls the window list instead, which drops the title once the close snapshot takes over.
for _ in $(seq 100); do
  [[ -z $("$UMBRIEL" windows --json | jq -c --arg title frame-one '.[] | select(.title == $title)') ]] && break
  sleep 0.02
done
if [[ -n $("$UMBRIEL" windows --json | jq -c --arg title frame-one '.[] | select(.title == $title)') ]]; then
  echo "the window never left the window list after window-close"
  exit 1
fi
grim "$IMAGE"
read -r ring_r6 ring_g6 ring_b6 < <(ring_pixel "$ring_x" "$ring_y")
if [[ "$ring_r5 $ring_g5 $ring_b5" != "$ring_r6 $ring_g6 $ring_b6" ]]; then
  echo "the close snapshot did not keep the frozen ring image: $ring_r5 $ring_g5 $ring_b5 -> $ring_r6 $ring_g6 $ring_b6"
  exit 1
fi
after_close=$(frames)
if (( after_close != before_close )); then
  echo "a frozen clock's close snapshot still produced effect-only frames"
  exit 1
fi
if ! "$UMBRIEL" windows > /dev/null; then
  echo "the compositor did not answer after closing a window under a frozen clock"
  exit 1
fi
"$UMBRIEL" clock-advance 5000
"$UMBRIEL" clock-resume
"$UMBRIEL" settle > /dev/null

# 3. effects.max_fps caps the render rate to its own due-frame interval: at 1 fps, two captures taken right after a
# due frame and 150ms apart never differ, but captures 1.3s apart do.
open_window
ring_x=$((x + w / 2))
ring_y=$((y - 12))
sed -i '/^\[effects\]$/a max_fps = 1' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
capped=$(frames)
for _ in $(seq 150); do
  now=$(frames)
  (( now > capped )) && break
  sleep 0.01
done
if (( now <= capped )); then
  echo "max_fps = 1 produced no due effect frame within 1.5s"
  exit 1
fi
grim "$IMAGE"
read -r ring_r7 ring_g7 ring_b7 < <(ring_pixel "$ring_x" "$ring_y")
sleep 0.15 # real time: no effect frame is due within max_fps = 1's 1000ms interval, so the ring must not change
grim "$IMAGE"
read -r ring_r8 ring_g8 ring_b8 < <(ring_pixel "$ring_x" "$ring_y")
if [[ "$ring_r7 $ring_g7 $ring_b7" != "$ring_r8 $ring_g8 $ring_b8" ]]; then
  echo "max_fps = 1 did not cap the render rate: the ring changed within 150ms ($ring_r7 $ring_g7 $ring_b7 -> $ring_r8 $ring_g8 $ring_b8)"
  exit 1
fi
sleep 1.15 # real time: effects.max_fps = 1 allows a due frame after its 1000ms interval elapses
grim "$IMAGE"
read -r ring_r9 ring_g9 ring_b9 < <(ring_pixel "$ring_x" "$ring_y")
if [[ "$ring_r7 $ring_g7 $ring_b7" == "$ring_r9 $ring_g9 $ring_b9" ]]; then
  echo "max_fps = 1 never let the ring advance: stayed at $ring_r7 $ring_g7 $ring_b7"
  exit 1
fi

# 4. Effect time keeps advancing while another animation drives the output's frames: a neighbour's 3s real-time
# close fade runs while two captures 600ms apart must show different time-keyed ring pixels.
cat "$BASE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = true
[animation.windows_in]
enabled = false
[animation.windows_move]
enabled = false
[animation.windows_out]
enabled = true
style = "fade"
duration_ms = 3000
curve = "linear"
[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
[effects]
border = "clock"
[effects.preset.clock]
kind = "border"
shader = "clock.glsl"
padding = 20
[[window_rule]]
match.title = "^frame-one$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^frame-two$"
default_floating = true
default_position = { x = 600, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
one_id=$id
open_window frame-two 0xFF0000FF
two_id=$id
two_x=$((x + w / 2))
two_y=$((y + h / 2))
"$UMBRIEL" msg "window-focus:$one_id" > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg "window-close:$two_id" > /dev/null
for _ in $(seq 100); do
  [[ -z $("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "frame-two")') ]] && break
  sleep 0.02
done
grim "$IMAGE"
read -r ring_r10 ring_g10 ring_b10 < <(ring_pixel "$ring_x" "$ring_y")
sleep 0.6 # real time: the close fade runs on the real clock, and effect time must move with it
grim "$IMAGE"
read -r ring_r11 ring_g11 ring_b11 < <(ring_pixel "$ring_x" "$ring_y")
read -r _ _ fading_b < <(ring_pixel "$two_x" "$two_y")
if (( fading_b < 20 )); then
  echo "the neighbour's close fade ended before the second capture, so the step proves nothing: blue=$fading_b"
  exit 1
fi
if [[ "$ring_r10 $ring_g10 $ring_b10" == "$ring_r11 $ring_g11 $ring_b11" ]]; then
  echo "effect time froze while another animation ran on the output: stayed at $ring_r10 $ring_g10 $ring_b10"
  exit 1
fi
echo "frozen-clock determinism, close-snapshot freezing, max_fps capping, and effect time under animations verified"
