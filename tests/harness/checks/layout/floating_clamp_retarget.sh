#!/usr/bin/env bash
# harness: outputs=1
# A usable-area change during a position animation retargets the tween into the new grabbable margin. The clamp used
# to be spent on the in-flight animation: the tween kept its old target and the window settled out of frame, and the
# change was never re-checked.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly TITLE=retarget-clamp
readonly PANEL_LOG="$UMBRIEL_RUNTIME_DIR/retarget-panel.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 2000
curve = "linear"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[[window_rule]]
match.title = "^retarget-clamp$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

ipc_box() {
  "$UMBRIEL" windows --json | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | "\(.x) \(.y) \(.w) \(.h)"'
}

wait_for_ready() {
  for _ in $(seq 80); do
    grep -q '^ready$' "$1" && return 0
    sleep 0.1
  done
  echo "exclusive-zone panel did not map: $(< "$1")"
  return 1
}

"$CLIENT" "$TITLE" 400 300 > "$UMBRIEL_RUNTIME_DIR/retarget-clamp.log" 2>&1 &
for _ in $(seq 60); do
  [[ -n $(ipc_box) ]] && break
  sleep 0.25
done
"$UMBRIEL" settle

# Tween toward the top-left clamp of the unshrunken usable area: the 400x300 window heads for x=-325, y=-225.
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg window-move-by:-9999/-9999 > /dev/null

# Map a 40px exclusive top panel mid-flight. The usable area's top edge moves to y=40, and the old target leaves only
# 35px of the window's height inside — the margin asks for 75px, so the tween must be re-aimed at y=-185.
"$LAYER_CLIENT" HEADLESS-1 40 > "$PANEL_LOG" 2>&1 &
panel=$!
wait_for_ready "$PANEL_LOG"

"$UMBRIEL" clock-advance 3000
"$UMBRIEL" clock-resume
"$UMBRIEL" settle

kill "$panel" 2> /dev/null || true
wait "$panel" 2> /dev/null || true

read -r wx wy ww wh < <(ipc_box)
visible_x=$((ww / 4))
((visible_x < 10)) && visible_x=10
((visible_x > 75)) && visible_x=75
visible_y=$((wh / 4))
((visible_y < 10)) && visible_y=10
((visible_y > 75)) && visible_y=75
min_x=$((visible_x - ww))
min_y=$((40 + visible_y - wh))
if ((wx < min_x || wy < min_y)); then
  echo "the tween settled outside the new grabbable margin: box: $(ipc_box)"
  echo "margin for a ${ww}x${wh} window under the 40px panel: x >= $min_x, y >= $min_y"
  exit 1
fi

echo "the usable change retargeted the tween into the new grabbable margin"
