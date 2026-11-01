#!/usr/bin/env bash
# harness: xcursor-theme=true
# Real title bar buttons select the pointing cursor and visibly contrast with light and dark backgrounds.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"

# Give the named pointing cursor a distinct size so the renderer can identify it without a host theme dependency.
python3 - "$XCURSOR_PATH" "$XCURSOR_THEME" <<'PY'
from pathlib import Path
import shutil, struct, sys
root = Path(sys.argv[1])
target = root / 'ssd-buttons' / 'cursors'
target.mkdir(parents=True)
shutil.copyfile(root / sys.argv[2] / 'cursors/default', target / 'default')
chunks, tocs, offset = [], [], 40
for size, width in [(24, 7), (48, 14)]:
    chunk = struct.pack('<9I', 36, 0xFFFD0002, size, 1, width, width, 0, 0, 0)
    chunk += struct.pack('<I', 0xFF00FF00) * width * width
    tocs.append(struct.pack('<3I', 0xFFFD0002, size, offset))
    chunks.append(chunk)
    offset += len(chunk)
(target / 'pointer').write_bytes(struct.pack('<4I', 0x72756358, 16, 0x00010000, 2) + b''.join(tocs + chunks))
PY
sed -i 's/^theme = .*/theme = "ssd-buttons"/' "$UMBRIEL_CONFIG"
cat >>"$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "forced"
height = 28
button_position = "right"
[colors.title_bar]
background = "#000000FF"
background_unfocused = "#000000FF"
close = "#663319FF"
maximize = "#663319FF"
[input.focus]
follows_mouse = false
[output.HEADLESS-1]
scale = 1
[[window_rule]]
match.title = "^button-hover-"
default_floating_size_px = { width = 300, height = 160 }
default_position = { x = 50, y = 70, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload >/dev/null
image=$UMBRIEL_RUNTIME_DIR/button-hover.png

cursor_at() {
  local x=$1 y=$2 size=$3 label=$4 state
  "$UMBRIEL_POINTER_CLIENT" "$output_w" "$output_h" move "$x" "$y"
  grim -c "$image"
  state=$("$UMBRIEL" cursor-state --json)
  jq -e --argjson size "$((size * scale))" '.image.texture_width == $size and .image.texture_height == $size' \
    <<<"$state" >/dev/null || {
    echo "$label: wrong rendered cursor: $state"
    exit 1
  }
}
button_red() {
  grim "$image"
  "$UMBRIEL_PIXEL_PROBE" "$image" pixel "$(((close_x - 3) * scale))" "$((button_y * scale))" | awk '{print $1}'
}

for scale in 1 2; do
  sed -i "s/^scale = .*/scale = $scale/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload >/dev/null
  read -r output_w output_h < <("$UMBRIEL" outputs --json | jq -r '.[0].logical_size | "\(.width) \(.height)"')
  for layout in scrolling stacking; do
    "$UMBRIEL" msg workspace-set-layout "$layout" >/dev/null
    for side in right left; do
      sed -i "s/^button_position = .*/button_position = \"$side\"/" "$UMBRIEL_CONFIG"
      "$UMBRIEL" msg config-reload >/dev/null
      for kind in normal fixed; do
        title=button-hover-$kind
        if [[ $kind == fixed ]]; then
          FIXED_SIZE=1 "$UMBRIEL_SUBSURFACE_CLIENT" "$title" 300 160 >"$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
        else
          "$UMBRIEL_UNMAP_CLIENT" "$title" 300 160 >"$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
        fi
        client=$!
        title_bar_wait_field "$title" title_bar true
        "$UMBRIEL" settle >/dev/null
        read -r x y w < <(title_bar_window "$title" | jq -r '"\(.x) \(.y) \(.w)"')
        empty_x=$((x + w / 2))
        button_y=$((y - 14))
        close_x=$((x + w - 14)) maximize_x=$((x + w - 36))
        if [[ $side == left ]]; then
          close_x=$((x + 14)) maximize_x=$((x + 36))
        fi
        cursor_at "$empty_x" "$button_y" 3 "empty title bar"
        normal=$(button_red)
        cursor_at "$close_x" "$button_y" 7 "close button"
        hovered=$(button_red)
        ((hovered > normal + 30)) || {
          echo "dark title bar hover was too faint: $normal to $hovered"
          exit 1
        }
        cursor_at "$maximize_x" "$button_y" "$([[ $kind == fixed ]] && echo 3 || echo 7)" "$kind maximize"
        cursor_at "$empty_x" "$button_y" 3 "leaving the buttons"

        # Verify the opposite contrast direction on a light bar, then return to the normal palette.
        sed -i 's/#000000FF/#FFFFFFFF/g' "$UMBRIEL_CONFIG"
        "$UMBRIEL" msg config-reload >/dev/null
        normal=$(button_red)
        cursor_at "$close_x" "$button_y" 7 "close button on a light bar"
        hovered=$(button_red)
        ((hovered < normal - 30)) || {
          echo "light title bar hover was too faint: $normal to $hovered"
          exit 1
        }
        pointer_hold "$output_w" "$output_h" press 272 -- move "$empty_x" "$button_y" release 272
        pressed=$(button_red)
        ((pressed < hovered - 10)) || {
          echo "pressed button was indistinguishable from hover"
          exit 1
        }
        pointer_release
        cursor_at "$empty_x" "$button_y" 3 "cancelled button press"
        title_bar_wait_field "$title" title_bar true
        sed -i 's/#FFFFFFFF/#000000FF/g' "$UMBRIEL_CONFIG"
        "$UMBRIEL" msg config-reload >/dev/null
        kill "$client"
        wait "$client" 2>/dev/null || true
        title_bar_wait_absent "$title"
      done
    done
  done
done
echo "title bar buttons show pointing cursors, stronger hover colors, and distinct pressed feedback"
