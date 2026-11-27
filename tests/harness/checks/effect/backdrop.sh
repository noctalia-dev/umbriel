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
OPAQUE_BUFFER=1 FILL_COLOR=0xFFFF0000 "$UMBRIEL_UNMAP_CLIENT" backdrop-opaque 300 200 > "$UMBRIEL_RUNTIME_DIR/opaque.log" 2>&1 &
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

sed -i '/^\[appearance.blur\]$/,/^\[/s/^enabled = true$/enabled = false/' "$UMBRIEL_CONFIG"
cat > "$UMBRIEL_RUNTIME_DIR/backdrop.glsl" <<'GLSL'
vec4 backdrop(vec2 uv) { return umbriel_sample(uv).brga; }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[appearance.title_bar]
mode = "forced"
height = 28
[colors.title_bar]
background = "#00000040"
background_unfocused = "#00000040"
[colors.overview]
background_tint = "#00FF00FF"
workspace_background = "#00FF00FF"
[[window_rule]]
match.title = "^backdrop-translucent$"
backdrop_effect = "off"
CONFIG
for state in disabled enabled excluded restored; do
  case $state in
    enabled) sed -i 's/^backdrop_enabled = false$/backdrop_enabled = true/' "$UMBRIEL_CONFIG" ;;
    excluded) sed -i 's/^in_capture = true$/in_capture = false/' "$UMBRIEL_CONFIG" ;;
    restored) sed -i 's/^in_capture = false$/in_capture = true/' "$UMBRIEL_CONFIG" ;;
  esac
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle > /dev/null
  read -r bx by < <("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "backdrop-opaque") | "\(.x + .w / 2) \(.y - 24)"')
  grim "$IMAGE"
  if [[ $state == disabled || $state == excluded ]]; then
    assert_region_rgb "$IMAGE" "3x3+$bx+$by" 0 191 0 "titlebar $state"
  else
    assert_region_rgb "$IMAGE" "3x3+$bx+$by" 0 0 191 "titlebar $state"
  fi
  assert_region_rgb "$IMAGE" "3x3+$ox+$oy" 255 0 0 "opaque client under titlebar"
done
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r cx cy cw ch < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'r > 0.98 && g < 0.02 && b < 0.02')
((cw > 0 && ch > 0))
assert_region_rgb "$IMAGE" "1x1+$((cx + cw / 2))+$((cy - 24 * cw / 300))" 0 0 191 "overview titlebar"
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle > /dev/null

cat > "$UMBRIEL_RUNTIME_DIR/backdrop.glsl" <<'GLSL'
vec4 backdrop(vec2 uv) { return vec4(0.0, 0.0, 0.75 + 0.25 * sin(umbriel_time), 1.0); }
GLSL
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" effect-frames --json | jq -e '[.outputs[].eligible] | add == 1' > /dev/null
"$UMBRIEL" clock-freeze
samples=()
for _ in 1 2 3; do
  "$UMBRIEL" clock-advance 1000
  grim "$IMAGE"
  read -r _ _ blue < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$bx" "$by")
  samples+=("$blue")
done
if [[ ${samples[0]} == "${samples[1]}" && ${samples[1]} == "${samples[2]}" ]]; then
  echo "titlebar backdrop did not advance without client commits"
  exit 1
fi
"$UMBRIEL" clock-resume
sed -i 's/^mode = "forced"$/mode = "off"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" effect-frames --json | jq -e '[.outputs[].eligible] | add == 0' > /dev/null

cat > "$UMBRIEL_RUNTIME_DIR/backdrop.glsl" <<'GLSL'
vec4 backdrop(vec2 uv) { return umbriel_sample(uv).brga; }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^popup-focus-regression$"
default_floating = true
default_floating_size_px = { width = 1260, height = 700 }
default_position = { x = 10, y = 10, anchor = "top_left" }
blur = false
blur_popups = false
backdrop_effect = "glass"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
POPUP_COLOR=0x80800000 "$UMBRIEL_POPUP_CLIENT" --unmap-on-close > "$UMBRIEL_RUNTIME_DIR/inherited-popup.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/inherited-popup.log" ready 1
grim "$IMAGE"
read -r px py popup_id < <("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "popup-focus-regression") | "\(.x) \(.y) \(.id)"')
"$UMBRIEL_POINTER_CLIENT" 1280 720 move "$((px + 200))" "$((py + 200))" click 272 > /dev/null
await_lines "$UMBRIEL_RUNTIME_DIR/inherited-popup.log" popup-mapped 1
px=$((px + 60)) py=$((py + 50))
for state in enabled excluded off restored disabled; do
  case $state in
    excluded) sed -i 's/^in_capture = true$/in_capture = false/' "$UMBRIEL_CONFIG" ;;
    off) sed -i 's/^in_capture = false$/in_capture = true/; s/^backdrop_effect = "glass"$/backdrop_effect = "off"/' "$UMBRIEL_CONFIG" ;;
    restored) sed -i '/^match.title = "\^popup-focus-regression\$"$/,$s/^backdrop_effect = "off"$/backdrop_effect = "glass"/' "$UMBRIEL_CONFIG" ;;
    disabled) sed -i 's/^backdrop_enabled = true$/backdrop_enabled = false/' "$UMBRIEL_CONFIG" ;;
  esac
  "$UMBRIEL" msg config-reload > /dev/null
  grim "$IMAGE"
  if [[ $state == enabled || $state == restored ]]; then
    assert_region_rgb "$IMAGE" "3x3+$px+$py" 213 42 59 "popup $state"
  else
    assert_region_rgb "$IMAGE" "3x3+$px+$py" 170 59 85 "popup $state"
  fi
done
sed -i 's/^backdrop_enabled = false$/backdrop_enabled = true/' "$UMBRIEL_CONFIG"
cat > "$UMBRIEL_RUNTIME_DIR/backdrop.glsl" <<'GLSL'
vec4 backdrop(vec2 uv) { return vec4(0.0, 0.0, 0.75 + 0.25 * sin(umbriel_time), 1.0); }
GLSL
"$UMBRIEL" msg config-reload > /dev/null
grim "$IMAGE"
"$UMBRIEL" effect-frames --json | jq -e '[.outputs[].eligible] | add > 0' > /dev/null
"$UMBRIEL" clock-freeze
samples=()
for _ in 1 2 3; do
  "$UMBRIEL" clock-advance 1000
  grim "$IMAGE"
  read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$px" "$py")
  ((red >= 125 && red <= 131 && green < 3))
  samples+=("$blue")
done
if [[ ${samples[0]} == "${samples[1]}" && ${samples[1]} == "${samples[2]}" ]]; then
  echo "popup backdrop did not advance without client commits"
  exit 1
fi
"$UMBRIEL" clock-resume
"$UMBRIEL" msg window-close "$popup_id" > /dev/null
await_lines "$UMBRIEL_RUNTIME_DIR/inherited-popup.log" unmapped 1
grim "$IMAGE"
"$UMBRIEL" effect-frames --json | jq -e '[.outputs[].eligible] | add == 0' > /dev/null

POPUP_COLOR=0x80800000 "$UMBRIEL_POPUP_CLIENT" --popup-on-stdin <<< open > "$UMBRIEL_RUNTIME_DIR/shutdown-popup.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/shutdown-popup.log" popup-mapped 1
grim "$IMAGE"
compositor_pid=$(cat "$UMBRIEL_RUNTIME_DIR/instance.pgid")
"$UMBRIEL" msg session-quit skip-confirmation > /dev/null 2>&1 || true
for _ in $(seq 100); do
  kill -0 "$compositor_pid" 2>/dev/null || break
  sleep 0.025
done
if kill -0 "$compositor_pid" 2>/dev/null; then
  echo "compositor did not exit with an inherited popup mapped"
  exit 1
fi

echo "titlebar and popup inheritance, capture, live reloads, overview, clocks, and teardown verified"
