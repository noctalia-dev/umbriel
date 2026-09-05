#!/usr/bin/env bash
# Observe shader-specific intermediate colors, source sampling, completion,
# closing snapshots, and automatic reload of an imported GLSL file.
set -euo pipefail
cat > "$UMBRIEL_RUNTIME_DIR/fixture-1.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }
GLSL

readonly CLIENT="$UMBRIEL_UNMAP_CLIENT"
readonly SHADER="${UMBRIEL_CONFIG%/*}/animation.glsl"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/shader.png"

cat > "$SHADER" <<'EOF'
vec4 animation(vec2 uv) {
    vec4 source = umbriel_sample(uv);
    return vec4(1.0, step(0.4, umbriel_progress), source.b > 0.01 ? 0.0 : 1.0, 1.0);
}
EOF
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = true
duration_ms = 1600
curve = "linear"
[animation.windows_in]
style = "fade"
shader = "animation.glsl"
[animation.windows_out]
shader = "fixture-1.glsl"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn() {
  "$CLIENT" "$1" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
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
  read -r red green blue < <(magick "$IMAGE" -crop "8x8+${sample_x}+${sample_y}" \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]\n' info:)
}

spawn shader-first
sleep 0.12
sample
if ! (( red > 220 && green < 30 && blue < 30 )); then
  echo "opening shader did not sample the client and produce red: $red $green $blue"
  exit 1
fi
sleep 0.65
sample
if ! (( red > 220 && green > 220 && blue < 30 )); then
  echo "shader progress did not transition red to yellow: $red $green $blue"
  exit 1
fi
sleep 1
sample
if ! (( blue > 80 && red < 200 )); then
  echo "opening shader did not release its target at completion: $red $green $blue"
  exit 1
fi

"$UMBRIEL" msg "window-close:$window_id" > /dev/null
sleep 0.15
sample
if ! (( green > 220 && red < 30 && blue < 30 )); then
  echo "closing snapshot did not run its shader: $red $green $blue"
  exit 1
fi
sleep 1.6

# Do not explicitly reload: the imported file must be a watcher dependency.
mark=$(wc -l < "$UMBRIEL_LOG")
cat > "$SHADER" <<'EOF'
vec4 animation(vec2 uv) { return vec4(0.0, 1.0, 1.0, 1.0); }
EOF
for _ in $(seq 80); do
  if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q 'config reloaded'; then
    break
  fi
  sleep 0.05
done
if ! tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q 'config reloaded'; then
  echo "editing the shader file did not trigger configuration reload"
  exit 1
fi
spawn shader-reloaded
sleep 0.12
sample
if ! (( red < 30 && green > 220 && blue > 220 )); then
  echo "new transition did not use edited shader source: $red $green $blue"
  exit 1
fi
echo "shader sampling, progress, completion, closing snapshot, and file reload verified"
