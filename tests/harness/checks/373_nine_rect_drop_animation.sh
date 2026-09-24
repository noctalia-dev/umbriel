#!/usr/bin/env bash
# harness: outputs=1
# A reinserted tile must animate from its dragged size, never an unarranged 1px slot.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly THEME="$(realpath examples/nine-rect/frame.png)"
cat >> "$UMBRIEL_CONFIG" <<EOF_CONFIG
[appearance]
use_nine_rect = true
[appearance.nine_rect]
texture = "$THEME"
slice_px = {top=16,bottom=16,left=16,right=16}
content_inset_px = {top=12,bottom=12,left=12,right=12}
[appearance.shadow]
enabled = false
[animation.windows_in]
enabled = false
[animation.windows_move]
duration_ms = 1000
curve = "linear"
[layout]
mode = "scrolling" # drop-test-mode
EOF_CONFIG
"$UMBRIEL" msg config-reload >/dev/null
FILL_COLOR=0xFFFF0000 RESIZE_FILL_COLOR=0xFFFF0000 "$CLIENT" drop-animation 600 600 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq 'length') == 1 ]] && break
  sleep 0.025
done
"$UMBRIEL" settle
capture() {
  grim "$UMBRIEL_RUNTIME_DIR/$1.png"
  local bx by bw bh
  read -r bx by bw bh < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/$1.png" bbox 'r > 0.59 && g < 0.2 && b < 0.2')
  (( bw > 150 && bh > 150 )) || { echo "$1 collapsed to $bw x $bh"; exit 1; }
}
for layout in scrolling dwindle master; do
  sed -i "s/^mode = .* # drop-test-mode/mode = \"$layout\" # drop-test-mode/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload >/dev/null
  "$UMBRIEL" settle
  for edge in 20 1260; do
    read -r x y w h < <("$UMBRIEL" windows --json | jq -r '.[] | "\(.x) \(.y) \(.w) \(.h)"')
    "$UMBRIEL" clock-freeze
    pointer_hold 1280 720 move "$((x+w/2))" "$((y+h/2))" mod logo press 272 move "$edge" 360 -- release 272 mod none
    "$UMBRIEL" clock-advance 1000
    capture "$layout-$edge-held"
    pointer_release
    "$UMBRIEL" clock-advance 1
    capture "$layout-$edge-drop"
    for step in 100 300 600; do
      "$UMBRIEL" clock-advance "$step"
      capture "$layout-$edge-$step"
    done
    "$UMBRIEL" clock-resume
    "$UMBRIEL" settle
  done
done
echo "left and right edge drops preserve visible size throughout all three layouts' animations"
