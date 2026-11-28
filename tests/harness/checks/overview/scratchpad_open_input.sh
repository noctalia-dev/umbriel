#!/usr/bin/env bash
# A pad fading out inside its preview owns the pointer for as long as its image is on screen. The workspace card the
# pad covers must not receive that click: acting on it would close the overview onto a window the user cannot see, in
# the middle of the opening. The second opening, without the click, is the reference: a press that reached the
# workspace card closes the overview, and the settled frames then show the desktop on one side and the filmstrip on the
# other.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly WINDOW_TITLE=overview-pad-input-window
readonly PAD_TITLE=overview-pad-input-pad
readonly OUT="$UMBRIEL_RUNTIME_DIR/pad-input"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = true

[animation.windows_in]
enabled = false

[animation.windows_out]
enabled = false

[animation.overview]
enabled = true
duration_ms = 5000
curve = "linear"

[appearance]
border_width = 2
outer_border_width = 2

[appearance.title_bar]
mode = "on"

[appearance.blur]
enabled = false

[overview]
zoom = 0.5
background_blur = false
shortcuts = false

[[window_rule]]
match.title = "^overview-pad-input-pad$"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 100, y = 120, anchor = "top_left" }

[[window_rule]]
match.title = "^overview-pad-input-window$"
default_floating = true
default_floating_size_px = { width = 360, height = 240 }
default_position = { x = 60, y = 60, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

FILL_COLOR=0xFF996633 "$CLIENT" "$WINDOW_TITLE" 360 240 > "$UMBRIEL_RUNTIME_DIR/pad-input-window.log" 2>&1 &
FILL_COLOR=0xFF0000FF "$CLIENT" "$PAD_TITLE" 300 200 > "$UMBRIEL_RUNTIME_DIR/pad-input-pad.log" 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -ge 2 ]] && break
  sleep 0.1
done
pad_id=$("$UMBRIEL" windows --json | jq -r --arg title "$PAD_TITLE" '.[] | select(.title == $title) | .id')
if [[ -z $pad_id ]]; then
  echo "pad client never mapped: $("$UMBRIEL" windows --json)"
  exit 1
fi
"$UMBRIEL" msg window-focus "$pad_id" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" settle > /dev/null

# Halfway through the opening the pad's card covers 235..460 x 180..330, its ring 230..465 x 175..335 and its title
# bar 235..460 x 159..180, inside the workspace window's card at 205..475 x 135..315: a press on any of them reaches the
# workspace card unless the pad card owns it.
grim "$OUT-closed.png"
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg overview-open > /dev/null
grim "$OUT-p00.png"
"$UMBRIEL" clock-advance 2500
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 232 255 click 272 > "$UMBRIEL_RUNTIME_DIR/pad-input-ring.log" 2>&1
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 300 170 click 272 > "$UMBRIEL_RUNTIME_DIR/pad-input-title.log" 2>&1
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 350 250 click 272 > "$UMBRIEL_RUNTIME_DIR/pad-input-pointer.log" 2>&1
"$UMBRIEL" clock-advance 2500
grim "$OUT-clicked.png"

# Settle the opening and close it on the frozen clock, then open again without a click.
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" clock-advance 2500
"$UMBRIEL" clock-advance 2500
"$UMBRIEL" clock-resume > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" clock-advance 5000
grim "$OUT-reference.png"

# A focused pad keeps the ring it was showing: its card must reproduce that ring, not the unfocused one, or the swap
# from the desktop to the filmstrip changes colour on the first frame. The ring runs 95..100, the content 100..400.
read -r band band_pixels < <("$UMBRIEL_PIXEL_PROBE" "$OUT-p00.png" diff "$OUT-closed.png" "10x10+94+215")
if ((band > 8)); then
  echo "the pad card did not keep the focused ring its member was drawing: largest difference $band"
  exit 1
fi
read -r content content_pixels < <("$UMBRIEL_PIXEL_PROBE" "$OUT-p00.png" diff "$OUT-closed.png" "298x198+101+121")
if ((content > 8)); then
  echo "the pad card did not reproduce the live pad at progress 0: largest difference $content"
  exit 1
fi

read -r largest differing < <("$UMBRIEL_PIXEL_PROBE" "$OUT-clicked.png" diff "$OUT-reference.png")
if ((largest > 2)); then
  echo "a click on the fading pad closed the overview onto the workspace card it covered: largest difference $largest"
  exit 1
fi

echo "a click on the fading pad image went nowhere"
