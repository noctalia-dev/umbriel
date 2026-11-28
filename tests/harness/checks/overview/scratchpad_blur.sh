#!/usr/bin/env bash
# A pad's blur is part of the workspace image it leaves inside its preview. For as long as the pad's card fades, the
# preview the pad covered carries the blur at the opening's strength, so the window beside the pad stays soft where the
# opening without the pad shows it sharp. The blur follows the opening's clock and leaves with it.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly WINDOW_TITLE=overview-pad-blur-window
readonly PAD_TITLE=overview-pad-blur-pad
readonly OUT="$UMBRIEL_RUNTIME_DIR/pad-blur"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
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

[animation.scratchpad]
enabled = true
dim = 0.5
blur = true

[colors]
backdrop = "#204060FF"

[colors.overview]
background_tint = "#00000000"
workspace_background = "#101820FF"

[appearance]
border_width = 2
outer_border_width = 2

[appearance.blur]
enabled = true
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0

[overview]
zoom = 0.5
background_blur = false
shortcuts = false

[[window_rule]]
match.title = "^overview-pad-blur-pad$"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
default_position = { x = 860, y = 230, anchor = "top_left" }

[[window_rule]]
match.title = "^overview-pad-blur-window$"
default_floating = true
default_floating_size_px = { width = 360, height = 240 }
default_position = { x = 60, y = 60, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

FILL_COLOR=0xFF996633 "$CLIENT" "$WINDOW_TITLE" 360 240 > "$UMBRIEL_RUNTIME_DIR/pad-blur-window.log" 2>&1 &
FILL_COLOR=0xFF0000FF "$CLIENT" "$PAD_TITLE" 420 260 > "$UMBRIEL_RUNTIME_DIR/pad-blur-pad.log" 2>&1 &
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

"$UMBRIEL" clock-freeze
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" clock-advance 2500
grim "$OUT-p50.png"
"$UMBRIEL" clock-advance 2000
grim "$OUT-p90.png"
"$UMBRIEL" clock-advance 500
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" clock-advance 2500
"$UMBRIEL" clock-advance 2500
"$UMBRIEL" clock-resume > /dev/null
"$UMBRIEL" settle > /dev/null

# The same opening without the pad is the sharp reference at the same progress: the window sits at 60,60 and its card
# ends at x=475 halfway through the opening, 519 at 90%.
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" clock-advance 2500
grim "$OUT-ref50.png"
"$UMBRIEL" clock-advance 2000
grim "$OUT-ref90.png"

read -r sharp_red sharp_green sharp_blue <<< "$("$UMBRIEL_PIXEL_PROBE" "$OUT-ref50.png" pixel 476 225)"
read -r blurred_red blurred_green blurred_blue <<< "$("$UMBRIEL_PIXEL_PROBE" "$OUT-p50.png" pixel 476 225)"
# Half of the pad's dim is left at this progress, so a sharp edge would read three quarters of the reference, 191 blue.
# The blur follows the same clock: at half alpha the soft edge sits halfway between that sharp edge and the fully
# blurred one, 52. A pad whose blur snapped to full strength reads below the band; an edge with no blur reads above it.
sharp=$((sharp_blue * 3 / 4))
if ((blurred_blue < sharp - 100 || blurred_blue > sharp - 60)); then
  echo "the pad's blur did not follow the opening: $blurred_blue against the sharp $sharp"
  exit 1
fi
# At 90% the blur has almost left: the same point just past the ring may only carry a trace of the window beside it. A
# blur still at full alpha reads 68 red there against the reference's 18.
read -r trace_red trace_green trace_blue <<< "$("$UMBRIEL_PIXEL_PROBE" "$OUT-p90.png" pixel 522 225)"
read -r sharp90_red sharp90_green sharp90_blue <<< "$("$UMBRIEL_PIXEL_PROBE" "$OUT-ref90.png" pixel 522 225)"
if ((trace_red > sharp90_red + 12)); then
  echo "the pad's blur did not end with the opening: $trace_red against $sharp90_red at 90%"
  exit 1
fi

echo "the pad's blur followed the opening inside its workspace preview"
