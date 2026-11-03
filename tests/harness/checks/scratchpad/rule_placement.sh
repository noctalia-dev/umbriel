#!/usr/bin/env bash
# harness: outputs=1
# A window that opens into a scratchpad through default_scratchpad takes its
# floating size, fullscreen, or maximize-to-edges state from its window rules,
# and a window moved into a scratchpad later keeps its size and position.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

windows() { "$UMBRIEL" windows --json; }

# wait_for <title> <jq predicate over the window as .>
wait_for() {
  local title=$1 predicate=$2 state=
  for _ in $(seq 80); do
    state=$(windows)
    if jq -e --arg title "$title" "any(.[]; .title == \$title and ($predicate))" <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "expected '$title' to satisfy '$predicate': $(jq -c --arg title "$title" '.[] | select(.title == $title)' <<< "$state")"
  exit 1
}

open() {
  RESIZE_FILL_COLOR=0xFF3366CC "$CLIENT" "$1" 200 150 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[scratchpad]]
name = "sized"

[[scratchpad]]
name = "full"

[[scratchpad]]
name = "edges"

[[scratchpad]]
name = "moved"

[[window_rule]]
match.title = "^placement-sized$"
default_scratchpad = "sized"
default_focused = true
default_floating_size = { width = 0.5, height = 0.5 }

[[window_rule]]
match.title = "^placement-full$"
default_scratchpad = "full"
default_focused = true
default_fullscreen = true

[[window_rule]]
match.title = "^placement-edges$"
default_scratchpad = "edges"
default_focused = true
default_maximize_to_edges = true

[[window_rule]]
match.title = "^placement-moved$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 40, y = 30, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

open placement-sized
wait_for placement-sized '.scratchpad == "sized" and .active and .fullscreen == false and .maximized_to_edges == false'
sized=$(windows | jq -c '.[] | select(.title == "placement-sized") | [.x, .y, .w, .h]')
read -r sx sy sw sh < <(jq -r '@tsv' <<< "$sized")
# Half the 1280x720 output in each direction, centered, give or take borders and gaps.
if ((sw < 560 || sw > 640 || sh < 300 || sh > 360)); then
  echo "default_floating_size did not size the scratchpad window: $sized"
  exit 1
fi
if ((sx + sw / 2 < 620 || sx + sw / 2 > 660 || sy + sh / 2 < 340 || sy + sh / 2 > 380)); then
  echo "the sized scratchpad window was not centered: $sized"
  exit 1
fi
"$UMBRIEL" msg scratchpad-toggle sized > /dev/null

open placement-full
wait_for placement-full '.scratchpad == "full" and .active and .fullscreen and .w == 1280 and .h == 720'
"$UMBRIEL" msg scratchpad-toggle full > /dev/null

open placement-edges
wait_for placement-edges '.scratchpad == "edges" and .active and .maximized_to_edges and .fullscreen == false'
"$UMBRIEL" msg scratchpad-toggle edges > /dev/null

open placement-moved
wait_for placement-moved '.scratchpad == "" and .floating and .w == 400 and .h == 300'
before=$(windows | jq -c '.[] | select(.title == "placement-moved") | [.x, .y, .w, .h]')
"$UMBRIEL" msg window-focus "$(windows | jq -r '.[] | select(.title == "placement-moved") | .id')" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad moved > /dev/null
"$UMBRIEL" msg scratchpad-toggle moved > /dev/null
wait_for placement-moved '.scratchpad == "moved" and .active and .fullscreen == false and .maximized_to_edges == false'
after=$(windows | jq -c '.[] | select(.title == "placement-moved") | [.x, .y, .w, .h]')
if [[ $before != "$after" ]]; then
  echo "moving a window into a scratchpad changed its geometry: $before -> $after"
  exit 1
fi

echo "rule-driven scratchpad size, fullscreen and maximize-to-edges applied at open; a moved-in window kept $after"
