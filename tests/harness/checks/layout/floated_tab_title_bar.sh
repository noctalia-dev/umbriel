#!/usr/bin/env bash
# A tab has no title bar in its group. Moved to the scratchpad or floated, it gets one, and the whole window, title bar
# and borders included, stays inside the usable area a panel leaves.
set -euo pipefail

readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly PANEL=40
readonly TITLE_BAR=28
readonly BORDER=2

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') == "$1" ]] && return 0
    sleep 0.1
  done
  echo "expected $1 window(s): $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" << EOF

[animation]
enabled = false

[layout]
gap = 0

[appearance]
border_width = $BORDER
outer_border_width = 0

[appearance.title_bar]
mode = "on"
height = $TITLE_BAR
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$LAYER_CLIENT" HEADLESS-1 "$PANEL" > "$UMBRIEL_RUNTIME_DIR/panel.log" 2>&1 &
spawn_client tab-a
wait_for_windows 1
for _ in $(seq 60); do
  (($("$UMBRIEL" windows --json | jq '.[0].y') > PANEL)) && break
  sleep 0.1
done
spawn_client tab-b
wait_for_windows 2
spawn_client tab-c
wait_for_windows 3
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg column-toggle-tabbed > /dev/null
"$UMBRIEL" settle

# The window's frame, from the top of its title bar to the bottom of its border, lies within the panel's bottom edge
# and the output's.
expect_inside() {
  "$UMBRIEL" settle
  local y h
  read -r y h <<< "$("$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | "\(.y) \(.h)"')"
  if ((y - TITLE_BAR - BORDER < PANEL || y + h + BORDER > 720)); then
    echo "$2: '$1' at y=$y h=$h reaches past the usable area ${PANEL}..720"
    exit 1
  fi
}

"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
"$UMBRIEL" msg scratchpad-toggle > /dev/null
expect_inside tab-c "moved to the scratchpad"

"$UMBRIEL" msg window-focus "$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "tab-b") | .id')" > /dev/null
"$UMBRIEL" msg window-toggle-floating > /dev/null
expect_inside tab-b "floated"

echo "a tab moved to the scratchpad or floated kept its title bar and borders inside the usable area"
