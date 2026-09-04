#!/usr/bin/env bash
# Each workspace preview mirrors the output's background- and bottom-layer surfaces. With `workspace_wallpaper` off
# the same pixels fall back to the flat `colors.overview.workspace_background` fill and the real bottom layer stays.
set -euo pipefail

readonly LAYER="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LAYER_LOG="$UMBRIEL_RUNTIME_DIR/wallpaper-background.log"
readonly BOTTOM_LOG="$UMBRIEL_RUNTIME_DIR/wallpaper-bottom.log"
readonly MIRRORED="$UMBRIEL_RUNTIME_DIR/overview-wallpaper-on.png"
readonly FLAT="$UMBRIEL_RUNTIME_DIR/overview-wallpaper-off.png"
readonly BASE_CONFIG="$UMBRIEL_RUNTIME_DIR/wallpaper-base.toml"

cp "$UMBRIEL_CONFIG" "$BASE_CONFIG"
cat >> "$BASE_CONFIG" <<'EOF'

[colors.overview]
background_tint = "#00000000"
workspace_background = "#FF0000FF"

[appearance]
corner_radius = 0

[animation.overview]
enabled = false

# Last section, so the second phase appends `workspace_wallpaper` to it.
[overview]
background_blur = false
EOF

cp "$BASE_CONFIG" "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

# The layer client fills a full-output background surface with 0xFF5577AA, and a 200x200 top-left bottom-layer surface
# with 0xFF00FF00.
start_layer() {
  local log=$1
  shift
  "$LAYER" "$@" > "$log" 2>&1 &
  for _ in $(seq 60); do
    grep -q '^ready$' "$log" && return 0
    sleep 0.02
  done
  echo "layer surface never presented: $(cat "$log")"
  exit 1
}

start_layer "$LAYER_LOG" HEADLESS-1 0
start_layer "$BOTTOM_LOG" HEADLESS-1 0 bottom-layer

# 1280x720 output at zoom 0.5: the only workspace row spans 320,180 to 960,540, so the bottom-layer surface's mirror
# lands in its top-left corner as a 100x100 square.
sample_rgb() {
  magick "$1" -crop "16x16+$2+$3" -format \
    '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

assert_rgb() {
  local label=$1 shot=$2 x=$3 y=$4 want_r=$5 want_g=$6 want_b=$7
  local r g b
  read -r r g b <<< "$(sample_rgb "$shot" "$x" "$y")"
  local dr=$((r - want_r)) dg=$((g - want_g)) db=$((b - want_b))
  dr=$((dr < 0 ? -dr : dr)); dg=$((dg < 0 ? -dg : dg)); db=$((db < 0 ? -db : db))
  if ((dr > 4 || dg > 4 || db > 4)); then
    echo "$label: sample at $x,$y is ($r,$g,$b), expected ($want_r,$want_g,$want_b)"
    exit 1
  fi
}

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.2
grim "$MIRRORED"
"$UMBRIEL" msg overview-close > /dev/null
assert_rgb "wallpaper mirrored" "$MIRRORED" 632 352 85 119 170
assert_rgb "bottom layer mirrored" "$MIRRORED" 344 204 0 255 0
# The real bottom layer is hidden while its copies stand in, so its own corner shows the wallpaper.
assert_rgb "bottom layer hidden" "$MIRRORED" 40 40 85 119 170

{ cat "$BASE_CONFIG"; printf 'workspace_wallpaper = false\n'; } > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.2
grim "$FLAT"
"$UMBRIEL" msg overview-close > /dev/null
assert_rgb "wallpaper disabled" "$FLAT" 632 352 255 0 0
assert_rgb "bottom layer kept" "$FLAT" 40 40 0 255 0

echo "workspace previews mirror the background- and bottom-layer surfaces, and fall back to the flat fill when disabled"
