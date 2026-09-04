#!/usr/bin/env bash
# A multilevel blur stores each reduced image in the top-left of a full-size effect texture. Sampling at the right and
# bottom of that reduced image must duplicate its edge texels rather than read the unused remainder of the texture.
set -euo pipefail

readonly LAYER="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LAYER_LOG="$UMBRIEL_RUNTIME_DIR/blur-background.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/overview-blur-edges.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors.overview]
background_tint = "#00000000"
workspace_background = "#00000000"

[appearance.blur]
enabled = true
optimized = true
passes = 3
radius = 12
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0

[overview]
background_blur = true
# The centre sample is the blur reference, so the workspace preview must not mirror the wallpaper over it.
workspace_wallpaper = false

[animation.overview]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$LAYER" HEADLESS-1 0 > "$LAYER_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^ready$' "$LAYER_LOG" && break
  sleep 0.02
done
if ! grep -q '^ready$' "$LAYER_LOG"; then
  echo "blur background layer never presented: $(cat "$LAYER_LOG")"
  exit 1
fi

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.2
grim "$SCREENSHOT"

sample_rgb() {
  magick "$SCREENSHOT" -crop "16x16+$1+$2" -format \
    '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

read -r center_r center_g center_b <<< "$(sample_rgb 632 352)"
assert_close() {
  local name=$1 x=$2 y=$3
  local r g b dr dg db
  read -r r g b <<< "$(sample_rgb "$x" "$y")"
  dr=$((r - center_r)); dr=$((dr < 0 ? -dr : dr))
  dg=$((g - center_g)); dg=$((dg < 0 ? -dg : dg))
  db=$((b - center_b)); db=$((db < 0 ? -db : db))
  if ((dr > 3 || dg > 3 || db > 3)); then
    echo "$name blur edge differs from center: center=($center_r,$center_g,$center_b) edge=($r,$g,$b)"
    exit 1
  fi
}

assert_close right 1264 352
assert_close bottom 632 704
assert_close corner 1264 704

echo "overview blur preserves a uniform background through the right and bottom edges"
