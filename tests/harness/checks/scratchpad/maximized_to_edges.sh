#!/usr/bin/env bash
# A tile maximized to the edges has no floating size of its own. Moved to a scratchpad, it fits the output with its
# borders instead of keeping a width that pushes the right border off screen.
set -euo pipefail

readonly TITLE=scratchpad-edges
readonly BORDER=4

cat >> "$UMBRIEL_CONFIG" <<EOF_CONFIG

[animation]
enabled = false

[appearance]
border_width = $BORDER
outer_border_width = 0
EOF_CONFIG
"$UMBRIEL" msg config-reload > /dev/null

box() {
  "$UMBRIEL" windows --json | jq -c --arg title "$TITLE" '.[] | select(.title == $title) | {x, y, w, h, floating}'
}

foot --title="$TITLE" sh -c 'sleep 120' > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ -n $(box) ]] && break
  sleep 0.1
done
"$UMBRIEL" settle

"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" settle

jq -e --argjson border "$BORDER" '
  .floating and .x >= $border and .y >= $border and .x + .w + $border <= 1280 and .y + .h + $border <= 720
' <<< "$(box)" > /dev/null || {
  echo "window with its ${BORDER}px border does not fit the 1280x720 output: $(box)"
  exit 1
}
