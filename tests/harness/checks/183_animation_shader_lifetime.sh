#!/usr/bin/env bash
# Imported examples, source replacement during an active transition, frozen
# close-during-open effects, and cancellation through the master switch.
set -euo pipefail
cat > "$UMBRIEL_RUNTIME_DIR/fixture-1.glsl" <<'GLSL'
vec4 animation(vec2 uv) { vec4 c = umbriel_sample(uv); return vec4(0.0, c.r * c.b, 0.0, c.a); }
GLSL
readonly BASE="$UMBRIEL_RUNTIME_DIR/lifetime-base.toml"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/lifetime.png"
readonly SOURCE="${UMBRIEL_CONFIG%/*}/lifetime.glsl"
readonly EXAMPLES="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../examples/shaders" && pwd)"
cp "$UMBRIEL_CONFIG" "$BASE"
cp "$EXAMPLES/reveal.glsl" "$SOURCE"
cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
duration_ms = 2400
curve = "linear"
[animation.windows_in]
style = "none"
shader = "lifetime.glsl"
[animation.windows_move]
shader = "$EXAMPLES/squash.glsl"
[animation.windows_out]
shader = "fixture-1.glsl"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn() {
  "$UMBRIEL_UNMAP_CLIENT" "$1" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]]
  window_id=$(jq -r .id <<< "$window")
  sample_x=$(jq -r '.x + (.w / 2 | floor)' <<< "$window")
  sample_y=$(jq -r '.y + (.h / 2 | floor)' <<< "$window")
}

sample() {
  grim "$IMAGE"
  read -r red green blue < <(magick "$IMAGE" -crop "4x4+${sample_x}+${sample_y}" \
    -format '%[fx:round(mean.r*255)] %[fx:round(mean.g*255)] %[fx:round(mean.b*255)]\n' info:)
}

spawn shader-example
sleep 0.3
sample
if ! (( red < 10 && green < 10 && blue < 10 )); then
  echo "reveal example did not mask the center during its opening: $red $green $blue"
  exit 1
fi
sleep 1.2
sample
if ! (( blue > 130 && red > 50 && red < 120 )); then
  echo "reveal example did not expose the center as progress advanced: $red $green $blue"
  exit 1
fi
sleep 1

cat > "$SOURCE" <<'EOF'
vec4 animation(vec2 uv) { return vec4(1.0, 0.0, 1.0, 1.0); }
EOF
"$UMBRIEL" msg config-reload > /dev/null
spawn shader-retained
sleep 0.12
sample
if ! (( red > 220 && green < 30 && blue > 220 )); then
  echo "initial program did not render magenta: $red $green $blue"
  exit 1
fi
cat > "$SOURCE" <<'EOF'
vec4 animation(vec2 uv) { return vec4(0.0, 1.0, 1.0, 1.0); }
EOF
"$UMBRIEL" msg config-reload > /dev/null
sleep 0.12
sample
if ! (( red > 220 && green < 30 && blue > 220 )); then
  echo "source reload replaced a program during its active transition: $red $green $blue"
  exit 1
fi
"$UMBRIEL" msg "window-close:$window_id" > /dev/null
sleep 0.12
sample
if ! (( red < 30 && green > 220 && blue < 30 )); then
  echo "closing shader did not sample the frozen opening effect: $red $green $blue"
  exit 1
fi
sleep 2.4
spawn shader-new-program
sleep 0.12
sample
if ! (( red < 30 && green > 220 && blue > 220 )); then
  echo "next transition did not adopt the new program: $red $green $blue"
  exit 1
fi
cat "$BASE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null
sleep 0.15
sample
# The shader must disappear immediately. An existing native fade may still
# finish its timeline, so check the client's color ratios before full brightness.
if ! (( red > 0 && green < blue && blue < 3 * red )); then
  echo "disabling animations did not remove the cyan shader: $red $green $blue"
  exit 1
fi
sleep 2.4
sample
if ! (( blue > 130 && red > 50 && red < 120 && green < 180 )); then
  echo "disabling animations did not restore ordinary client pixels: $red $green $blue"
  exit 1
fi
if grep -q 'Animation shader .*rejected' "$UMBRIEL_LOG"; then
  echo "a bundled example or lifetime shader failed compilation"
  exit 1
fi
echo "bundled shaders, retained programs, frozen close effects, and disabling verified"
