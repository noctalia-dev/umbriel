#!/usr/bin/env bash
# harness: outputs=2
# Pushing the pointer past an outer output edge runs the edge action once the over-travel reaches the resistance. It
# fires once per excursion and re-arms after the pointer leaves the edge. Shared edges and fullscreen windows never fire.
set -euo pipefail

pointer() { "$UMBRIEL_POINTER_CLIENT" 2560 720 "$@"; }
action() { "$UMBRIEL" msg "$@" > /dev/null; }
windows() { "$UMBRIEL" windows --json; }
active_title() {
  windows | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end'
}
wait_active() {
  local actual=
  for _ in $(seq 80); do
    actual=$(active_title)
    [[ $actual == "$1" ]] && return 0
    sleep 0.025
  done
  echo "expected $1 active, got $actual"
  return 1
}
stays_active() {
  "$UMBRIEL" settle > /dev/null
  [[ $(active_title) == "$1" ]] || { echo "expected $1 to stay active, got $(active_title)"; return 1; }
}
focus() {
  action window-focus "$(windows | jq -r --arg title "$1" '.[] | select(.title == $title) | .id')"
  wait_active "$1"
}

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[input.focus]
follows_mouse = true
[output.HEADLESS-1]
position = [0, 0]
scale = 1
[output.HEADLESS-2]
position = [1280, 0]
scale = 1
[hot_edges.left]
enabled = true
resistance_px = 300
action = "window-focus-left"
[hot_edges.right]
enabled = true
resistance_px = 300
action = "window-focus-right"
[[window_rule]]
match.title = "^edge-"
default_output = "HEADLESS-1"
CONFIG
action config-reload
pointer move 640 360

for title in edge-a edge-b; do
  "$UMBRIEL_UNMAP_CLIENT" "$title" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  wait_active "$title"
done

# Below the resistance nothing happens, then the rest of the push fires once.
pointer move 5 360 relative -100 0 relative -100 0
stays_active edge-b
pointer relative -60 0 relative -60 0
wait_active edge-a

# Still latched: more pushing at the same edge does not fire again.
focus edge-b
pointer relative -500 0 relative -500 0
stays_active edge-b

# Moving off the edge re-arms it.
pointer relative 50 0 relative -400 0
wait_active edge-a

# A vertical-dominant push at the left edge counts nothing.
focus edge-b
pointer move 5 360 relative -20 -400 relative -20 -400
stays_active edge-b

# Crossing the shared edge to the other output does not fire either edge. Hover focus leaves the window.
pointer move 1270 360 relative 200 0 relative 400 0
"$UMBRIEL" settle > /dev/null
[[ $(active_title) != edge-a ]] || { echo "shared edge fired"; exit 1; }
pointer relative -400 0 relative -600 0
"$UMBRIEL" settle > /dev/null
[[ $(active_title) != edge-a ]] || { echo "shared edge fired"; exit 1; }
focus edge-b

# A fullscreen window on the output suppresses the edge.
action window-toggle-fullscreen
"$UMBRIEL" settle > /dev/null
pointer move 5 360 relative -400 0 relative -400 0
stays_active edge-b
action window-toggle-fullscreen
"$UMBRIEL" settle > /dev/null
pointer move 5 360 relative -400 0 relative -400 0
wait_active edge-a
focus edge-b

# Disabled by default: removing the section stops the action.
sed -i '/^\[hot_edges/,/^action = "window-focus-right"/d' "$UMBRIEL_CONFIG"
action config-reload
pointer move 5 360 relative -400 0 relative -400 0
stays_active edge-b

echo 'edge push runs the action once per excursion and stays quiet on shared edges and fullscreen'
