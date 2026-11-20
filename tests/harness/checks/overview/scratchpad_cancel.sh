#!/usr/bin/env bash
# A cancelled overview hands the pad back as it was. The overview hides every visible pad when it opens, so a close that
# never lets the opening settle has to restore the pad, the one window it showed and the focus it held. The pad holds two
# windows and shows one of them, so a restore that loses that window shows both.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly WINDOW_TITLE=overview-pad-cancel-window
readonly PAD_TITLE=overview-pad-cancel-pad
readonly OUT="$UMBRIEL_RUNTIME_DIR/pad-cancel"

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
blur = false

[colors]
backdrop = "#204060FF"

[colors.overview]
background_tint = "#00000000"
workspace_background = "#101820FF"

[appearance]
border_width = 2
outer_border_width = 2

[appearance.blur]
enabled = false

[overview]
zoom = 0.5
background_blur = false
shortcuts = false

[[window_rule]]
match.title = "^overview-pad-cancel-pad$"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
default_position = { x = 860, y = 230, anchor = "top_left" }

[[window_rule]]
match.title = "^overview-pad-cancel-window$"
default_floating = true
default_floating_size_px = { width = 360, height = 240 }
default_position = { x = 60, y = 60, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

FILL_COLOR=0xFF996633 "$CLIENT" "$WINDOW_TITLE" 360 240 > "$UMBRIEL_RUNTIME_DIR/pad-cancel-window.log" 2>&1 &
FILL_COLOR=0xFF0000FF "$CLIENT" "$PAD_TITLE" 420 260 > "$UMBRIEL_RUNTIME_DIR/pad-cancel-pad.log" 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -ge 2 ]] && break
  sleep 0.1
done
pad_id=$("$UMBRIEL" windows --json | jq -r --arg title "$PAD_TITLE" '.[] | select(.title == $title) | .id')
window_id=$("$UMBRIEL" windows --json | jq -r --arg title "$WINDOW_TITLE" '.[] | select(.title == $title) | .id')
if [[ -z $pad_id || -z $window_id ]]; then
  echo "clients never mapped: $("$UMBRIEL" windows --json)"
  exit 1
fi
for id in "$pad_id" "$window_id"; do
  "$UMBRIEL" msg window-focus "$id" > /dev/null
  "$UMBRIEL" msg window-move-to-scratchpad > /dev/null
done
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" msg scratchpad-window-show-next > /dev/null
"$UMBRIEL" settle > /dev/null

shown=$("$UMBRIEL" windows --json | jq -r '.[] | select(.scratchpad == "default" and .visible) | .title')
if [[ $(grep -c . <<< "$shown") -ne 1 ]]; then
  echo "the pad did not settle on one window: $("$UMBRIEL" windows --json)"
  exit 1
fi
read -r shown_id shown_x shown_y shown_w shown_h < <(
  "$UMBRIEL" windows --json | jq -r --arg title "$shown" '.[] | select(.title == $title) | "\(.id) \(.x) \(.y) \(.w) \(.h)"'
)
"$UMBRIEL" msg window-focus "$shown_id" > /dev/null
"$UMBRIEL" settle > /dev/null
if ! "$UMBRIEL" windows --json | jq -e --arg title "$shown" \
    'any(.[] | select(.title == $title); .visible and .active)' > /dev/null; then
  echo "the pad's window did not take focus for the cancel: $("$UMBRIEL" windows --json)"
  exit 1
fi
grim "$OUT-padfocused.png"

# Open and close before the opening settles, so the overview never commits its dismissal.
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" clock-advance 1000
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" clock-advance 2500
"$UMBRIEL" clock-advance 2500
"$UMBRIEL" clock-resume > /dev/null
"$UMBRIEL" settle > /dev/null
restored=$("$UMBRIEL" windows --json | jq -r '.[] | select(.scratchpad == "default" and .visible) | .title')
if [[ $restored != "$shown" ]] \
    || ! "$UMBRIEL" windows --json | jq -e --arg title "$shown" \
        'any(.[] | select(.title == $title); .active)' > /dev/null; then
  echo "a cancelled overview did not hand the pad back on its own window focused: $("$UMBRIEL" windows --json)"
  exit 1
fi
grim "$OUT-restored.png"
read -r largest differing < <(
  "$UMBRIEL_PIXEL_PROBE" "$OUT-restored.png" diff "$OUT-padfocused.png" \
    "$((shown_w - 2))x$((shown_h - 2))+$((shown_x + 1))+$((shown_y + 1))"
)
if ((largest > 8)); then
  echo "a cancelled overview did not put the pad back on screen: largest difference $largest"
  exit 1
fi

echo "a cancelled overview handed the pad back on the window it showed, focused"
