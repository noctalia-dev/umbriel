#!/usr/bin/env bash
# Output-relative reveals survive every transform, fractional scale and nonzero layout origins.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
readonly BASE="$UMBRIEL_RUNTIME_DIR/base.toml"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/geometry.png"
workspace_reveal_config
cp "$UMBRIEL_CONFIG" "$BASE"
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFFFF0000 RESIZE_FILL_COLOR=0xFFFF0000 "$UMBRIEL_UNMAP_CLIENT" geometry-a 1280 720 > "$UMBRIEL_RUNTIME_DIR/a.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/a.log" mapped 1
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" settle
FILL_COLOR=0xFF0000FF RESIZE_FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" geometry-b 1280 720 > "$UMBRIEL_RUNTIME_DIR/b.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/b.log" mapped 1
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
"$UMBRIEL" settle
for transform in normal 90 180 270 flipped flipped-90 flipped-180 flipped-270; do
  cp "$BASE" "$UMBRIEL_CONFIG"
  cat >> "$UMBRIEL_CONFIG" <<CONFIG
[output.HEADLESS-1]
position = [137, -53]
scale = 1.25
transform = "$transform"
workspace_axis = "horizontal"
CONFIG
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" clock-freeze
  "$UMBRIEL" msg workspace-switch:1 > /dev/null
  "$UMBRIEL" clock-advance 1000
  "$UMBRIEL" clock-resume
  "$UMBRIEL" settle
  "$UMBRIEL" clock-freeze
  "$UMBRIEL" msg workspace-switch:2 > /dev/null
  "$UMBRIEL" clock-advance 500
  grim -s 1 -o HEADLESS-1 "$IMAGE"
  IFS=x read -r width height < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" size)
  left=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$((width / 4))" "$((height / 2))")
  right=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$((width * 3 / 4))" "$((height / 2))")
  [[ $left == '0 0 255' && $right == '255 0 0' ]] || {
    echo "$transform: logical reveal orientation failed, $width x $height: left=$left right=$right"; exit 1;
  }
  "$UMBRIEL" clock-advance 1000
  "$UMBRIEL" clock-resume
  "$UMBRIEL" settle
done
# Changing geometry during a held reveal must leave the native active destination intact.
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg workspace-switch:1 > /dev/null
"$UMBRIEL" clock-advance 500
sed -i 's/^scale = 1.25$/scale = 1.5/; s/^transform = .*/transform = "normal"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" clock-advance 1000
"$UMBRIEL" clock-resume
"$UMBRIEL" settle
grim -s 1 -o HEADLESS-1 "$IMAGE"
IFS=x read -r width height < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" size)
[[ $("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$((width / 2))" "$((height / 2))") == '255 0 0' ]]
echo 'eight transforms, fractional scale, nonzero origin and mid-reveal geometry change verified'
