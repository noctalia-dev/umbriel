#!/usr/bin/env bash
# A released window's deferred re-home must not demote the drag tree while
# another drag is in progress. The shared drag tree is elevated over
# fullscreen once per drag; if the settling window's departure lowers it, the
# still-held window drops behind the fullscreen window until its own release.
# The animation clock freezes before the release, so the settling window cannot
# fall out of the overlap window by wall-clock luck.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
# dragme's grab point (its center) tracks the pointer; sample well inside it.
readonly SAMPLE="80x80+600+320"
readonly FLOOR=2000
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/settle-overlap.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "floating"

[input.focus]
follows_mouse = false

[colors]
backdrop = "#000000FF"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
drag_opacity = 1.0
[appearance.shadow]
enabled = false
[animation]
duration_ms = 1
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
[animation.windows_drag]
physics = true
[[window_rule]]
match.title = "^fullscreen-main$"
default_floating = false
default_fullscreen = true
[[window_rule]]
match.title = "^settler$"
default_floating = true
default_position = { x = 100, y = 200, anchor = "top_left" }
[[window_rule]]
match.title = "^dragme$"
default_floating = true
default_focused = true
default_position = { x = 800, y = 120, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_window() {
  local title=$1
  for _ in $(seq 80); do
    "$UMBRIEL" windows --json | jq -e --arg title "$title" 'any(.[]; .title == $title)' > /dev/null && return 0
    sleep 0.025
  done
  echo "window $title did not map: $("$UMBRIEL" windows --json)"
  return 1
}

map_window() {
  local title=$1 w=$2 h=$3 color=$4
  FILL_COLOR=$color "$UMBRIEL_UNMAP_CLIENT" "$title" "$w" "$h" \
    > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  wait_for_window "$title"
  "$UMBRIEL" settle > /dev/null
}

map_window fullscreen-main 1280 720 0xFF0000FF
map_window settler 300 220 0xFFFF0000
map_window dragme 300 220 0xFF00FF00

green_count() {
  "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.9 && r < 0.1 && b < 0.1' "$SAMPLE"
}

# One pointer session: the first hold stops mid-drag of settler, the second
# holds dragme over the fullscreen window. The releases run only when this
# check kicks the client.
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move 250 310 mod super press "$BTN_LEFT" \
  move 310 310 move 370 310 move 430 310 \
  -- release "$BTN_LEFT" mod none \
  move 950 230 mod super press "$BTN_LEFT" \
  move 875 263 move 800 296 move 725 330 move 640 360 \
  mark bgrab hold \
  release "$BTN_LEFT" mod none

# Freeze before the release: from here on the only ticks are the ones this
# check orders, so settler is still settling when dragme's grab lands.
"$UMBRIEL" clock-freeze
pointer_step bgrab

"$UMBRIEL" clock-advance 16 > /dev/null
grim -o HEADLESS-1 "$IMAGE"
held=$(green_count)
echo "held drag over fullscreen: green=$held"

# The settling sheet's spring has long settled on these ticks: its deferred
# re-home lands while dragme is still held in the drag tree.
"$UMBRIEL" clock-advance 64 > /dev/null
"$UMBRIEL" clock-advance 256 > /dev/null
"$UMBRIEL" clock-advance 512 > /dev/null
"$UMBRIEL" clock-advance 1024 > /dev/null
grim -o HEADLESS-1 "$IMAGE"
overlap=$(green_count)
echo "after the overlapping settle: green=$overlap"
"$UMBRIEL" clock-resume
pointer_release

if ((held < FLOOR)); then
  echo "scenario never engaged: the held window does not draw over fullscreen (green=$held)"
  exit 1
fi
if ((overlap < FLOOR)); then
  echo "the settling window's re-home demoted the drag tree mid-drag: the held window fell behind fullscreen (held=$held overlap=$overlap)"
  exit 1
fi
echo "a settling re-home leaves an in-progress drag elevated"
