#!/usr/bin/env bash
# harness: outputs=2
# Logical UVs must survive fractional scale and rotation. An overview shader
# samples the result of the window shader, and neither may bleed to its neighbour.
set -euo pipefail
cat > "$UMBRIEL_RUNTIME_DIR/fixture-1.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return uv.x < 0.5 ? vec4(1.0,0.0,0.0,1.0) : vec4(0.0,0.0,1.0,1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/fixture-2.glsl" <<'GLSL'
vec4 animation(vec2 uv) { vec4 c = umbriel_sample(uv); return vec4(0.0,c.r,0.0,c.a); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/fixture-3.glsl" <<'GLSL'
this is not GLSL
GLSL
readonly BASE="$UMBRIEL_RUNTIME_DIR/composition-base.toml"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/composition.png"
cp "$UMBRIEL_CONFIG" "$BASE"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output."HEADLESS-1"]
scale = 1.25
transform = "90"
[output."HEADLESS-2"]
scale = 1.25
transform = "90"
[animation]
duration_ms = 4000
curve = "linear"
[animation.windows_in]
shader = "fixture-1.glsl"
[animation.overview]
shader = "fixture-2.glsl"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL_UNMAP_CLIENT" shader-composition 700 700 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "shader-composition")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
home=$(jq -r '.workspace | split(":")[0]' <<< "$window")
if [[ $home == HEADLESS-1 ]]; then neighbour=HEADLESS-2; else neighbour=HEADLESS-1; fi
sleep 0.12
grim -s 1 -o "$home" "$IMAGE"
# The client may still be resizing toward its layout target. Locate its drawn
# rectangle, then test orientation within that actual intermediate frame.
read -r width height x y < <(magick "$IMAGE" -colorspace gray -threshold 1% -trim -format '%w %h %X %Y\n' info:)
x=${x#+}
y=${y#+}
left=$((x + width / 4))
right=$((x + 3 * width / 4))
middle=$((y + height / 2))
read -r red blue < <(magick "$IMAGE" -crop "4x4+$left+$middle" -format '%[fx:round(mean.r*255)] %[fx:round(mean.b*255)]\n' info:)
if ! (( red > 220 && blue < 30 )); then
  echo "logical left side was not red on rotated/scaled output: $red $blue"
  exit 1
fi
read -r red blue < <(magick "$IMAGE" -crop "4x4+$right+$middle" -format '%[fx:round(mean.r*255)] %[fx:round(mean.b*255)]\n' info:)
if ! (( blue > 220 && red < 30 )); then
  echo "logical right side was not blue on rotated/scaled output: $red $blue"
  exit 1
fi
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.25
grim -s 1 -o "$home" "$IMAGE"
green=$(magick "$IMAGE" -fx '(g > 0.9 && r < 0.1 && b < 0.1) ? 1 : 0' -format '%[fx:round(mean*w*h)]' info:)
if (( green < 1000 )); then
  echo "outer shader did not sample the inner window effect: $green green pixels"
  exit 1
fi
grim -s 1 -o "$neighbour" "$IMAGE"
green=$(magick "$IMAGE" -fx '(g > 0.9 && r < 0.1 && b < 0.1) ? 1 : 0' -format '%[fx:round(mean*w*h)]' info:)
if (( green > 10 )); then
  echo "window effect escaped to neighbouring output: $green green pixels"
  exit 1
fi
"$UMBRIEL" msg overview-close > /dev/null
sleep 4.2

# An invalid program must produce a labelled compiler error and leave the
# compositor able to map another client through its built-in animation.
cat "$BASE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation.windows_in]
shader = "fixture-3.glsl"
EOF
"$UMBRIEL" msg config-reload > /dev/null
if ! grep -q "Animation shader .*rejected; using built-in animation" "$UMBRIEL_LOG"; then
  echo "invalid GLSL did not produce the expected fallback diagnostic"
  exit 1
fi
"$UMBRIEL_UNMAP_CLIENT" shader-fallback 700 700 > "$UMBRIEL_RUNTIME_DIR/fallback.log" 2>&1 &
for _ in $(seq 60); do
  if "$UMBRIEL" windows --json | jq -e '.[] | select(.title == "shader-fallback")' > /dev/null; then
    echo "rotated/scaled UVs, nested sampling, containment, and invalid-source fallback verified"
    exit 0
  fi
  sleep 0.05
done
echo "client did not map after shader compilation failure"
exit 1
