#!/usr/bin/env bash
# Dwindle window movement follows screen direction rather than depth-first leaf order. In A | (B above C), moving C
# left must enter A's tile, not move upward into B's, and A must keep its place instead of trading it with C. Leaving
# and re-entering the right-hand split also returns focus to the tile that was focused there last, not to the
# geometrically nearest one. A directional swap afterwards exchanges two windows between their tiles.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

spawn_client() {
  local title=$1
  "$CLIENT" "$title" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_focus() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$want" '.[] | select(.id == $id) | .focused') == true ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $want to be focused: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "dwindle"

[animation]
duration_ms = 1
curve = "linear"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client dwindle-move-left
wait_for_count 1
spawn_client dwindle-move-upper-right
wait_for_count 2
spawn_client dwindle-move-lower-right
wait_for_count 3
sleep 0.1

windows=$("$UMBRIEL" windows --json)
left_x=$(jq -r '.[] | select(.title == "dwindle-move-left") | .x' <<< "$windows")
left_y=$(jq -r '.[] | select(.title == "dwindle-move-left") | .y' <<< "$windows")
upper_x=$(jq -r '.[] | select(.title == "dwindle-move-upper-right") | .x' <<< "$windows")
upper_y=$(jq -r '.[] | select(.title == "dwindle-move-upper-right") | .y' <<< "$windows")
lower_id=$(jq -r '.[] | select(.title == "dwindle-move-lower-right") | .id' <<< "$windows")
lower_x=$(jq -r '.[] | select(.title == "dwindle-move-lower-right") | .x' <<< "$windows")
lower_y=$(jq -r '.[] | select(.title == "dwindle-move-lower-right") | .y' <<< "$windows")
if [[ -z $lower_id || $left_x -ge $lower_x || $upper_x -ne $lower_x || $upper_y -ge $lower_y ]]; then
  echo "expected one left tile and two vertically stacked right tiles: $windows"
  exit 1
fi

"$UMBRIEL" msg window-focus "$lower_id" > /dev/null
wait_for_focus "$lower_id"

upper_id=$(jq -r '.[] | select(.title == "dwindle-move-upper-right") | .id' <<< "$windows")
left_id=$(jq -r '.[] | select(.title == "dwindle-move-left") | .id' <<< "$windows")
"$UMBRIEL" msg window-focus-left > /dev/null
wait_for_focus "$left_id"
"$UMBRIEL" msg window-focus-right > /dev/null
if ! wait_for_focus "$lower_id"; then
  echo "focus-right did not return to the last-focused tile of the right split (upper is $upper_id)"
  exit 1
fi
"$UMBRIEL" msg window-move-left > /dev/null

moved_x=$lower_x
for _ in $(seq 40); do
  moved_x=$("$UMBRIEL" windows --json | jq -r --arg id "$lower_id" '.[] | select(.id == $id) | .x')
  [[ $moved_x -lt $lower_x ]] && break
  sleep 0.1
done
if [[ $moved_x -ne $left_x ]]; then
  echo "expected lower-right window to move left from x=$lower_x to x=$left_x, got: $("$UMBRIEL" windows --json)"
  exit 1
fi

windows=$("$UMBRIEL" windows --json)
kept_x=$(jq -r --arg id "$left_id" '.[] | select(.id == $id) | .x' <<< "$windows")
kept_y=$(jq -r --arg id "$left_id" '.[] | select(.id == $id) | .y' <<< "$windows")
moved_y=$(jq -r --arg id "$lower_id" '.[] | select(.id == $id) | .y' <<< "$windows")
if [[ $kept_x -ne $left_x || $kept_y -ne $left_y || $moved_y -le $kept_y ]]; then
  echo "expected the moved window below the left window, which keeps its place: $windows"
  exit 1
fi

echo "Dwindle window movement follows horizontal screen geometry"

"$UMBRIEL" msg window-swap-right > /dev/null
for _ in $(seq 40); do
  windows=$("$UMBRIEL" windows --json)
  swapped_x=$(jq -r --arg id "$lower_id" '.[] | select(.id == $id) | .x' <<< "$windows")
  upper_now_x=$(jq -r --arg id "$upper_id" '.[] | select(.id == $id) | .x' <<< "$windows")
  upper_now_y=$(jq -r --arg id "$upper_id" '.[] | select(.id == $id) | .y' <<< "$windows")
  [[ $swapped_x -eq $upper_x && $upper_now_x -eq $left_x && $upper_now_y -eq $moved_y ]] && break
  sleep 0.1
done
if [[ $swapped_x -ne $upper_x || $upper_now_x -ne $left_x || $upper_now_y -ne $moved_y ]]; then
  echo "window-swap-right did not exchange the moved window with the right-hand tile: $windows"
  exit 1
fi

echo "Dwindle directional swap exchanges windows between tiles"

# Four corners: the moved window over the new one on the right, A over B on the left. Focusing the upper-right window
# last makes it the right split's remembered tile, which a swap from the lower-left must still ignore.
spawn_client dwindle-move-fourth
wait_for_count 4
fourth_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "dwindle-move-fourth") | .id')
"$UMBRIEL" msg window-focus "$lower_id" > /dev/null
wait_for_focus "$lower_id"
"$UMBRIEL" msg window-focus "$upper_id" > /dev/null
wait_for_focus "$upper_id"
"$UMBRIEL" msg window-swap-right > /dev/null
for _ in $(seq 40); do
  windows=$("$UMBRIEL" windows --json)
  upper_now_x=$(jq -r --arg id "$upper_id" '.[] | select(.id == $id) | .x' <<< "$windows")
  upper_now_y=$(jq -r --arg id "$upper_id" '.[] | select(.id == $id) | .y' <<< "$windows")
  fourth_x=$(jq -r --arg id "$fourth_id" '.[] | select(.id == $id) | .x' <<< "$windows")
  [[ $upper_now_x -eq $upper_x && $upper_now_y -eq $moved_y && $fourth_x -eq $left_x ]] && break
  sleep 0.1
done
if [[ $upper_now_x -ne $upper_x || $upper_now_y -ne $moved_y || $fourth_x -ne $left_x ]]; then
  echo "window-swap-right from the lower-left did not swap with the lower-right window: $windows"
  exit 1
fi

echo "Dwindle directional swap stays on the same row"
