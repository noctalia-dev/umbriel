#!/usr/bin/env bash
# A compositor window move (Mod+left-click) held at the scrolling edge must slide the strip under the dragged window
# until an offscreen column is reachable, and dropping there must place the window after that column.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

spawn_client() {
  foot --title="move-edge-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

window_x() {
  jq -r --arg title "$1" '.[] | select(.title == $title) | .x' <<< "$2"
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout.scrolling]
default_extent_fraction = 0.5
center_focused = "never"
EOF
"$UMBRIEL" msg config-reload > /dev/null

for id in $(seq 1 4); do
  spawn_client "$id"
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $id ]] && break
    sleep 0.25
  done
done
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
initial_last_x=$(window_x move-edge-4 "$windows")
if ((initial_last_x < OUTPUT_W)); then
  echo "the last column began inside the viewport instead of offscreen: $windows"
  exit 1
fi
start_x=$(jq -r '.[] | select(.title == "move-edge-1") | (.x + .w / 2 | round)' <<< "$windows")
start_y=$(jq -r '.[] | select(.title == "move-edge-1") | (.y + .h / 2 | round)' <<< "$windows")

pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  mod logo move "$start_x" "$start_y" press "$BTN_LEFT" move 1279 360 \
  -- release "$BTN_LEFT" mod none

# The pointer never moves again, so only the edge timer can reveal the last column.
revealed=false
last_x=$initial_last_x
for _ in $(seq 120); do
  windows=$("$UMBRIEL" windows --json)
  last_x=$(window_x move-edge-4 "$windows")
  if ((last_x <= OUTPUT_W / 2 + 60)); then
    revealed=true
    break
  fi
  sleep 0.05
done
if [[ $revealed != true ]]; then
  echo "window move at the right edge did not reveal the offscreen column: x $initial_last_x -> $last_x"
  pointer_release
  exit 1
fi

pointer_release
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
moved_x=$(window_x move-edge-1 "$windows")
last_x=$(window_x move-edge-4 "$windows")
if ((moved_x <= last_x)); then
  echo "dropping at the revealed edge did not place move-edge-1 after move-edge-4: $windows"
  exit 1
fi

echo "window move scrolled to an offscreen column and dropped after it"
