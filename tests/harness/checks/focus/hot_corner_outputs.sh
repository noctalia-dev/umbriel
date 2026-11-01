#!/usr/bin/env bash
# harness: outputs=3
# A fullscreen output blocks its corners even while another monitor owns keyboard focus.
set -euo pipefail

pointer() { "$UMBRIEL_POINTER_CLIENT" 3584 720 "$@"; }
action() { "$UMBRIEL" msg "$@" > /dev/null; }
windows() { "$UMBRIEL" windows --json; }
workspace() {
  "$UMBRIEL" workspaces --json | jq -r --arg output "$1" '.[] | select(.output == $output and .active) | .index'
}
wait_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(windows | jq -r --arg title "$title" --arg field "$field" '.[] | select(.title == $title) | .[$field]')
    [[ $actual == "$expected" ]] && return 0
    sleep 0.025
  done
  echo "$title: expected $field=$expected, got $actual"
  return 1
}
focus() {
  local id
  id=$(windows | jq -r --arg title "$1" '.[] | select(.title == $title) | .id')
  action window-focus "$id"
  wait_field "$1" active true
}
wait_workspace() {
  local output=$1 expected=$2 actual=
  for _ in $(seq 80); do
    actual=$(workspace "$output")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.025
  done
  echo "$output: expected workspace $expected, got $actual"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[input.focus]
follows_mouse = false
[output.HEADLESS-1]
position = [0, 0]
scale = 1
workspaces = 3
[output.HEADLESS-2]
position = [1280, 0]
scale = 1.25
workspaces = 3
[output.HEADLESS-3]
position = [2304, 0]
scale = 1
workspaces = 3
[hot_corners.top_left]
enabled = true
delay_ms = 0
action = "workspace-next"
[hot_corners.top_right]
enabled = true
delay_ms = 0
action = "workspace-next"
[[window_rule]]
match.title = "^corner-left$"
default_output = "HEADLESS-1"
[[window_rule]]
match.title = "^corner-middle-"
default_output = "HEADLESS-2"
[[window_rule]]
match.title = "^corner-middle-game$"
default_fullscreen = true
[[window_rule]]
match.title = "^corner-right$"
default_output = "HEADLESS-3"
CONFIG
action config-reload
pointer move 600 360
for title in corner-left corner-middle-game corner-right; do
  env FILL_COLOR=0xFF3388CC RESIZE_FILL_COLOR=0xFF3388CC "$UMBRIEL_UNMAP_CLIENT" "$title" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  wait_field "$title" focused true
done
wait_field corner-middle-game fullscreen true
"$UMBRIEL" settle > /dev/null
if ! "$UMBRIEL" outputs --json | jq -e '
  any(.[]; .name == "HEADLESS-2" and .position == {x: 1280, y: 0}
    and .logical_size == {width: 1024, height: 576})
' > /dev/null; then
  echo "middle output did not use the expected logical geometry"
  exit 1
fi
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/fullscreen.png"
pixel=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/fullscreen.png" pixel 100 10)
[[ $pixel == "51 136 204" ]] || { echo "fullscreen content missing at the top: $pixel"; exit 1; }

for follows in false true; do
  for delay in 0 75; do
    sed -i "s/^follows_mouse = .*/follows_mouse = $follows/; s/^delay_ms = .*/delay_ms = $delay/" "$UMBRIEL_CONFIG"
    action config-reload
    for side in left right; do
      if [[ $side == left ]]; then
        from_x=1260
        corner_x=1281
      else
        from_x=2324
        corner_x=2302
      fi
      pointer move "$from_x" 2
      focus "corner-$side"
      wait_field corner-middle-game focused true
      # The pause observes the real dwell timer, including the delayed-corner case.
      pointer move "$corner_x" 2 pause 180
      if [[ $(workspace HEADLESS-2) != 1 ]]; then
        echo "fullscreen destination hot corner fired (side=$side, follows_mouse=$follows, delay_ms=$delay)"
        exit 1
      fi
      wait_field corner-middle-game fullscreen true
    done

    # A fullscreen game on the middle monitor must not disable another output's corner.
    pointer move 600 360
    focus corner-middle-game
    pointer move 1 2 pause 180
    wait_workspace HEADLESS-1 2
    wait_workspace HEADLESS-2 1
    action workspace-switch 1 --output HEADLESS-1
  done
done

# A retained fullscreen flag on an inactive workspace must not block the current one.
action workspace-switch 2 --output HEADLESS-2
pointer move 600 360
focus corner-left
pointer move 1281 2 pause 180
wait_workspace HEADLESS-2 3
wait_field corner-middle-game fullscreen true

# The destination's current regular focus wins over an older fullscreen window there.
sed -i 's/^follows_mouse = .*/follows_mouse = false/' "$UMBRIEL_CONFIG"
action config-reload
action workspace-switch 1 --output HEADLESS-2
pointer move 1792 288
"$UMBRIEL_UNMAP_CLIENT" corner-middle-foreground > "$UMBRIEL_RUNTIME_DIR/foreground.log" 2>&1 &
wait_field corner-middle-foreground fullscreen false
focus corner-middle-foreground
wait_field corner-middle-foreground focused true
wait_field corner-middle-game fullscreen true
pointer move 600 360
focus corner-left
pointer move 1281 2 pause 180
wait_workspace HEADLESS-2 2

echo 'fullscreen suppression follows the destination output across shared edges, scaling, dwell times, and focus modes'
