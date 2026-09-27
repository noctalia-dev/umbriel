#!/usr/bin/env bash
# Each workspace preview mirrors the output's background- and bottom-layer surfaces. With `workspace_wallpaper` off
# the same pixels fall back to the flat `colors.overview.workspace_background` fill and the real bottom layer stays.
set -euo pipefail

readonly LAYER="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LAYER_LOG="$UMBRIEL_RUNTIME_DIR/wallpaper-background.log"
readonly BOTTOM_LOG="$UMBRIEL_RUNTIME_DIR/wallpaper-bottom.log"
readonly MIRRORED="$UMBRIEL_RUNTIME_DIR/overview-wallpaper-on.png"
readonly FLAT="$UMBRIEL_RUNTIME_DIR/overview-wallpaper-off.png"
readonly HORIZONTAL="$UMBRIEL_RUNTIME_DIR/overview-wallpaper-horizontal.png"
readonly HORIZONTAL_FLAT="$UMBRIEL_RUNTIME_DIR/overview-wallpaper-horizontal-flat.png"
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
"$UMBRIEL" settle
grim "$MIRRORED"
"$UMBRIEL" msg overview-close > /dev/null
assert_rgb "wallpaper mirrored" "$MIRRORED" 632 352 85 119 170
assert_rgb "bottom layer mirrored" "$MIRRORED" 344 204 0 255 0
# The real bottom layer is hidden while its copies stand in, so its own corner shows the wallpaper.
assert_rgb "bottom layer hidden" "$MIRRORED" 40 40 85 119 170

{ cat "$BASE_CONFIG"; printf 'workspace_wallpaper = false\n'; } > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
grim "$FLAT"
"$UMBRIEL" msg overview-close > /dev/null
assert_rgb "wallpaper disabled" "$FLAT" 632 352 255 0 0
assert_rgb "bottom layer kept" "$FLAT" 40 40 0 255 0

# Horizontally arranged workspaces put the adjacent preview beside the active one instead of below it, so a preview's
# mirrors travel along X with it. Two workspaces, and no shortcut badges to sit on the sampled corners.
write_horizontal_config() {
  {
    cat "$BASE_CONFIG"
    printf 'shortcuts = false\nworkspace_wallpaper = %s\n' "$1"
    cat <<'EOF'

[output."HEADLESS-1"]
workspaces = 2
workspace_axis = "horizontal"
EOF
  } > "$UMBRIEL_CONFIG"
}

write_horizontal_config true
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
grim "$HORIZONTAL"
"$UMBRIEL" msg overview-close > /dev/null
# The active preview still spans 320,180 to 960,540, and the second steps 640 + 0.1 * 1280 * 0.5 = 704 px along X, so
# it enters the output at x = 1024 with the same vertical band. Each preview mirrors the bottom-layer surface into its
# own top-left corner as a 100x100 square, which is where the copies become traceable.
assert_rgb "active bottom layer mirrored" "$HORIZONTAL" 344 204 0 255 0
assert_rgb "adjacent bottom layer mirrored" "$HORIZONTAL" 1060 210 0 255 0
# Nothing steps along Y: where a vertically stacked second preview would have put its copy, the wallpaper is all there
# is. The real bottom layer is still hidden while the copies stand in.
assert_rgb "no bottom layer copy below" "$HORIZONTAL" 360 630 85 119 170
assert_rgb "bottom layer hidden" "$HORIZONTAL" 40 40 85 119 170

# With the mirrors off, each preview shows the flat fill instead, which draws the preview's own bounds: the adjacent
# fill sits to the right, and it stops at the gap rather than bleeding across it.
write_horizontal_config false
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
grim "$HORIZONTAL_FLAT"
"$UMBRIEL" msg overview-close > /dev/null
assert_rgb "active fill" "$HORIZONTAL_FLAT" 632 352 255 0 0
assert_rgb "adjacent fill beside" "$HORIZONTAL_FLAT" 1100 352 255 0 0
assert_rgb "gap between previews" "$HORIZONTAL_FLAT" 990 352 85 119 170
assert_rgb "nothing below the previews" "$HORIZONTAL_FLAT" 632 620 85 119 170

echo "workspace previews mirror the background- and bottom-layer surfaces, and fall back to the flat fill when disabled"
