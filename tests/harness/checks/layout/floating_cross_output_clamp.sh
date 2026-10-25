#!/usr/bin/env bash
# harness: outputs=2
# A cross-output move restores a floating window proportionally to the destination's usable area. The proportional
# origin can resolve outside the grabbable margin on a smaller output (a scale jump can leave only a sliver on
# screen), so the restored origin is clamped: floatingKeepVisible (a quarter of the window, 10..75px) stays inside
# the usable area per axis, the same margin a free drop settles into.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly TITLE=cross-clamp

BASELINE=$(< "$UMBRIEL_CONFIG")
{
  printf '%s\n' "$BASELINE"
  printf '\n[animation]\nenabled = false\n'
  printf '\n[output.HEADLESS-1]\nposition = [0, 0]\n'
  printf '\n[output.HEADLESS-2]\nscale = 2\n'
  cat << 'EOF'

[[window_rule]]
match.title = "^cross-clamp$"
default_floating = true
default_output = "HEADLESS-1"
default_floating_size_px = { width = 400, height = 300 }
EOF
} > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

output_scale() {
  "$UMBRIEL" outputs | awk -v name="$1" '$1 == name { found = 1; next } found && /Scale:/ { print $2; exit }'
}

wait_for_output_scale() {
  local actual=
  for _ in $(seq 40); do
    actual=$(output_scale "$1")
    [[ $actual == "$2" ]] && return 0
    sleep 0.1
  done
  echo "$1 did not settle at scale $2, got $actual"
  "$UMBRIEL" outputs
  return 1
}

ipc_box() {
  "$UMBRIEL" windows --json | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | "\(.x) \(.y) \(.w) \(.h)"'
}

workspace_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | .workspace'
}

# HEADLESS-2 is the 1280x720 headless mode at scale 2: a 640x360 logical output sitting at x=1280, right of the
# scale-1 HEADLESS-1.
wait_for_output_scale HEADLESS-2 2.000000

"$CLIENT" "$TITLE" 400 300 > "$UMBRIEL_RUNTIME_DIR/cross-clamp.log" 2>&1 &
for _ in $(seq 60); do
  [[ -n $(ipc_box) ]] && break
  sleep 0.25
done
"$UMBRIEL" settle

# Park the float at the right-bottom clamp of its own output: x=1205/1280 and y=645/720 of the usable area.
"$UMBRIEL" msg window-move-by:9999/9999 > /dev/null
"$UMBRIEL" settle
read -r sx sy sw sh < <(ipc_box)
if ((sx != 1205 || sy != 645)); then
  echo "the float did not park at the source margin: $(ipc_box)"
  exit 1
fi

# The same fractions on the scaled output land at x=603, y=323 of its 640x360 usable area, where only 37px and 37px
# of the 400x300 window stay visible; the margin asks for 75px per axis.
"$UMBRIEL" msg window-move-to-output-right > /dev/null
"$UMBRIEL" settle

ws=$(workspace_of)
if [[ $ws != HEADLESS-2:* ]]; then
  echo "the window did not move to HEADLESS-2: workspace $ws"
  exit 1
fi

read -r wx wy ww wh < <(ipc_box)
visible_x=$((ww / 4))
((visible_x < 10)) && visible_x=10
((visible_x > 75)) && visible_x=75
visible_y=$((wh / 4))
((visible_y < 10)) && visible_y=10
((visible_y > 75)) && visible_y=75
min_x=$((1280 + visible_x - ww))
max_x=$((1280 + 640 - visible_x))
min_y=$((visible_y - wh))
max_y=$((360 - visible_y))
if ((wx < min_x || wx > max_x || wy < min_y || wy > max_y)); then
  echo "the cross-output restore left the window outside the grabbable margin: box: $(ipc_box)"
  echo "margin for a ${ww}x${wh} window on the 640x360 output: x in [$min_x, $max_x], y in [$min_y, $max_y]"
  exit 1
fi

echo "the cross-output restore landed inside the grabbable margin"
