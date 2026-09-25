#!/usr/bin/env bash
# A tiled close snapshot must finish its windows_out effect even when its configured windows_move timeline is much
# shorter. The solid shader makes the snapshot lifetime directly observable after that timeline has completed.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/tiled-close.png"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/tiled-close-client.log"

cat > "$UMBRIEL_RUNTIME_DIR/tiled-close-green.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }
GLSL

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[animation.windows_in]
enabled = false

[animation.windows_out]
enabled = true
duration_ms = 1600
curve = "linear"
style = "fade"
shader = "tiled-close-green.glsl"

[animation.windows_move]
enabled = true
duration_ms = 150
curve = "linear"
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$UMBRIEL_UNMAP_CLIENT" tiled-close 800 500 > "$CLIENT_LOG" 2>&1 &
window=
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "tiled-close")')
  [[ -n $window ]] && break
  sleep 0.025
done
if [[ -z $window ]]; then
  echo "tiled close client did not map"
  exit 1
fi

sample_x=$(jq -r '.x + (.w / 2 | floor)' <<< "$window")
sample_y=$(jq -r '.y + (.h / 2 | floor)' <<< "$window")
window_id=$(jq -r .id <<< "$window")

# Animation time only moves by clock-advance; advancing 2000 ms finishes the 1600 ms windows_out timeline.
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg "window-close:$window_id" > /dev/null
for _ in $(seq 80); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.025
done
if ! grep -q '^unmapped$' "$CLIENT_LOG"; then
  echo "tiled close client did not unmap"
  exit 1
fi
for _ in $(seq 80); do
  if ! "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "tiled-close")' > /dev/null; then
    break
  fi
  sleep 0.025
done
if "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "tiled-close")' > /dev/null; then
  echo "compositor did not observe the tiled close client unmap"
  exit 1
fi

sample() {
  grim "$IMAGE"
  magick "$IMAGE" -crop "8x8+${sample_x}+${sample_y}" \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]\n' info:
}

# The configured windows_move timeline ended 200 ms ago, while windows_out is still near the start of its timeline.
"$UMBRIEL" clock-advance 350
read -r red green blue < <(sample)
if ! ((green > 220 && red < 30 && blue < 30)); then
  echo "tiled close effect ended with windows_move: sampled $red $green $blue while windows_out was active"
  exit 1
fi

# The snapshot remains owned by windows_out and must disappear when that timeline actually finishes.
"$UMBRIEL" clock-advance 2000
"$UMBRIEL" settle
read -r red green blue < <(sample)
if ! ((green < 30 && red < 30 && blue < 30)); then
  echo "tiled close snapshot remained after windows_out: sampled $red $green $blue"
  exit 1
fi

# An accepted overshooting curve may reach progress 1 before its clock ends. Keep the last nonzero coordinated box
# until the snapshot is reaped instead of hiding a constant-output custom shader at that early endpoint.
sed -i '0,/curve = "linear"/s//curve = "easeoutback"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

readonly OVERSHOOT_LOG="$UMBRIEL_RUNTIME_DIR/tiled-close-overshoot.log"
"$UMBRIEL_UNMAP_CLIENT" tiled-close-overshoot 800 500 > "$OVERSHOOT_LOG" 2>&1 &
overshoot_window=
for _ in $(seq 80); do
  overshoot_window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "tiled-close-overshoot")')
  [[ -n $overshoot_window ]] && break
  sleep 0.025
done
if [[ -z $overshoot_window ]]; then
  echo "overshooting tiled close client did not map"
  exit 1
fi

overshoot_id=$(jq -r .id <<< "$overshoot_window")
"$UMBRIEL" msg "window-close:$overshoot_id" > /dev/null
for _ in $(seq 80); do
  if ! "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "tiled-close-overshoot")' > /dev/null; then
    break
  fi
  sleep 0.025
done
if "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "tiled-close-overshoot")' > /dev/null; then
  echo "compositor did not observe the overshooting tiled close client unmap"
  exit 1
fi

"$UMBRIEL" clock-advance 800
grim "$IMAGE"
read -r red green blue < <(magick "$IMAGE" \
  -format '%[fx:round(255*maxima.r)] %[fx:round(255*maxima.g)] %[fx:round(255*maxima.b)]\n' info:)
if ! ((green > 220 && red < 30 && blue < 30)); then
  echo "overshooting windows_out curve hid its active snapshot early: sampled peak $red $green $blue"
  exit 1
fi

"$UMBRIEL" clock-advance 2000
"$UMBRIEL" settle
grim "$IMAGE"
read -r red green blue < <(magick "$IMAGE" \
  -format '%[fx:round(255*maxima.r)] %[fx:round(255*maxima.g)] %[fx:round(255*maxima.b)]\n' info:)
if ! ((green < 30 && red < 30 && blue < 30)); then
  echo "overshooting tiled close snapshot remained after windows_out: sampled peak $red $green $blue"
  exit 1
fi

echo "tiled close effects outlived shorter configured windows_move timelines and early curve endpoints, then finished"
