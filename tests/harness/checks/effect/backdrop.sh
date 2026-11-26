#!/usr/bin/env bash
# Backdrop shaders sample before native blur and client pixels, survive reloads, and stop requesting frames when off.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/backdrop.png"
cat > "$UMBRIEL_RUNTIME_DIR/backdrop.glsl" <<'GLSL'
vec4 backdrop(vec2 uv) { return umbriel_sample(uv).brga; }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 24
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
optimized = false
passes = 2
radius = 4
noise = 0.0
brightness = 0.0
contrast = 1.0
saturation = 1.0
[colors]
backdrop = "#00FF00FF"
[effects]
in_capture = true
[effects.preset.glass]
kind = "backdrop"
shader = "backdrop.glsl"
[[window_rule]]
match.title = "^backdrop-"
default_floating = true
blur = false
backdrop_effect = "glass"
[[window_rule]]
match.title = "^backdrop-translucent$"
default_position = { x = 100, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^backdrop-opaque$"
default_position = { x = 600, y = 100, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0x80800000 "$UMBRIEL_UNMAP_CLIENT" backdrop-translucent 300 200 > "$UMBRIEL_RUNTIME_DIR/translucent.log" 2>&1 &
FILL_COLOR=0xFFFF0000 "$UMBRIEL_UNMAP_CLIENT" backdrop-opaque 300 200 > "$UMBRIEL_RUNTIME_DIR/opaque.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/translucent.log" mapped 1
await_lines "$UMBRIEL_RUNTIME_DIR/opaque.log" mapped 1
"$UMBRIEL" settle > /dev/null
read -r x y w h < <("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "backdrop-translucent") | "\(.x) \(.y) \(.w) \(.h)"')
read -r ox oy ow oh < <("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "backdrop-opaque") | "\(.x) \(.y) \(.w) \(.h)"')
x=$((x + w / 2)) y=$((y + h / 2))
ox=$((ox + ow / 2)) oy=$((oy + oh / 2))

for native in disabled enabled; do
  if [[ $native == enabled ]]; then
    sed -i '/^\[appearance.blur\]$/,/^\[/s/^enabled = false$/enabled = true/; s/^blur = false$/blur = true/' "$UMBRIEL_CONFIG"
    "$UMBRIEL" msg config-reload > /dev/null
    "$UMBRIEL" settle > /dev/null
  fi
  grim "$IMAGE"
  read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
  if ((r < 110 || r > 145 || g > 15 || b < 110 || b > 145)); then
    echo "native blur $native: backdrop did not sample raw green behind the red client: $r $g $b"
    exit 1
  fi
  read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$((x - w / 2 + 1))" "$((y - h / 2 + 1))")
  if ((r > 15 || g < 240 || b > 15)); then
    echo "native blur $native: backdrop escaped the rounded client corner: $r $g $b"
    exit 1
  fi
  read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$ox" "$oy")
  if ((r < 240 || g > 15 || b > 15)); then
    echo "native blur $native: backdrop overwrote opaque foreground pixels: $r $g $b"
    exit 1
  fi
done
"$UMBRIEL" effect-frames --json | jq -e '[.outputs[].eligible] | add == 0' > /dev/null

sed -i 's/^in_capture = true$/in_capture = false/; s/^blur = true$/blur = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
if ((r < 110 || r > 145 || g < 110 || g > 145 || b > 15)); then
  echo "excluding the backdrop from capture did not preserve the unblurred client: $r $g $b"
  exit 1
fi

sed -i 's/^in_capture = false$/in_capture = true/; s/^blur = false$/blur = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
if ((r < 110 || r > 145 || g > 15 || b < 110 || b > 145)); then
  echo "including the backdrop in capture did not restore the shader: $r $g $b"
  exit 1
fi

sed -i 's/^backdrop_effect = "glass"$/backdrop_effect = "off"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
if ((r < 110 || r > 145 || g > 15 || b > 15)); then
  echo "off did not restore native blur's black backdrop: $r $g $b"
  exit 1
fi

sed -i 's/^backdrop_effect = "off"$/backdrop_effect = "glass"/' "$UMBRIEL_CONFIG"
cat > "$UMBRIEL_RUNTIME_DIR/backdrop.glsl" <<'GLSL'
vec4 backdrop(vec2 uv) { return invalid_backdrop_shader; }
GLSL
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
if ((r < 110 || r > 145 || g > 15 || b > 15)); then
  echo "an invalid shader did not restore native blur: $r $g $b"
  exit 1
fi

cat > "$UMBRIEL_RUNTIME_DIR/backdrop.glsl" <<'GLSL'
vec4 backdrop(vec2 uv) { return vec4(0.0, 0.0, 0.75 + 0.25 * sin(umbriel_time), 1.0); }
GLSL
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" effect-frames --json | jq -e '[.outputs[].eligible] | add > 0' > /dev/null
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 1000
grim "$IMAGE"
read -r _ _ first < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
"$UMBRIEL" clock-advance 2000
grim "$IMAGE"
read -r r g second < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
"$UMBRIEL" clock-advance 1000
grim "$IMAGE"
read -r _ _ third < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
if (((first == second && second == third) || r < 110 || r > 145 || g > 15 || second < 45)); then
  echo "repaired time-reading backdrop did not advance behind unchanged client pixels: $first -> $r $g $second"
  exit 1
fi
"$UMBRIEL" clock-resume

sed -i 's/^backdrop_effect = "glass"$/backdrop_effect = "off"/; s/^blur = true$/blur = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
if ((r < 110 || r > 145 || g < 110 || g > 145 || b > 15)); then
  echo "disabling the animated effect did not restore the plain transparent window: $r $g $b"
  exit 1
fi
"$UMBRIEL" effect-frames --json | jq -e '[.outputs[].eligible] | add == 0' > /dev/null
"$UMBRIEL" effects --json | jq -e '.presets[] | select(.name == "glass") | .state == "unreferenced"' > /dev/null
before=$("$UMBRIEL" effect-frames --json | jq '[.outputs[].effect_frames] | add')
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 1000
"$UMBRIEL" clock-resume
"$UMBRIEL" settle > /dev/null
after=$("$UMBRIEL" effect-frames --json | jq '[.outputs[].effect_frames] | add')
if ((after != before)); then
  echo "disabled backdrop still requested effect frames: $before -> $after"
  exit 1
fi

sed -i 's/^backdrop_effect = "off"$/backdrop_effect = "glass"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" effect-frames --json | jq -e '[.outputs[].eligible] | add > 0' > /dev/null
sed -i '/^\[effects\]$/a backdrop_enabled = false' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
if ((r < 110 || r > 145 || g < 110 || g > 145 || b > 15)); then
  echo "the global switch did not restore the plain transparent window: $r $g $b"
  exit 1
fi
"$UMBRIEL" effect-frames --json | jq -e '[.outputs[].eligible] | add == 0' > /dev/null
"$UMBRIEL" effects --json | jq -e '.presets[] | select(.name == "glass") | .state == "unreferenced"' > /dev/null

echo "raw backdrop input, foreground preservation, native fallback, shader recovery, and disabled frame gating verified"
