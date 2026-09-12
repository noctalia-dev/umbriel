#!/usr/bin/env bash
# harness: outputs=2
# The `-next` and `-previous` output actions cycle rather than point: from the
# last monitor in layout order they wrap to the first, which no directional
# action does. Each assertion observes the transition after the wrap, so an
# implementation that merely walks to the adjacent monitor and stops fails here
# even though its first step looks correct.
set -euo pipefail

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    exit 1
  fi
}

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

focused_output() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.focused) | .output'
}

window_output() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .workspace | split(":")[0]'
}

wait_for_focused_output() {
  local expected=$1 actual=
  for _ in $(seq 50); do
    actual=$(focused_output)
    if [[ $actual == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected focus on $expected, got '$actual'"
  exit 1
}

wait_for_window_output() {
  local title=$1 expected=$2 actual=
  for _ in $(seq 50); do
    actual=$(window_output "$title")
    if [[ $actual == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$title' on $expected, got '$actual'"
  exit 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[output.HEADLESS-1]
position = [0, 0]

[output.HEADLESS-2]
position = [1280, 0]
EOF
"$UMBRIEL" msg config-reload > /dev/null

first=$(focused_output)
case $first in
  HEADLESS-1)
    other=HEADLESS-2
    edge_action=output-focus-left
    ;;
  HEADLESS-2)
    other=HEADLESS-1
    edge_action=output-focus-right
    ;;
  *)
    echo "unexpected initial focused output '$first'"
    exit 1
    ;;
esac

# The cycle starts on the monitor at the end of the layout, where the matching
# direction has nowhere to go.
if out=$("$UMBRIEL" msg "$edge_action" 2>&1); then
  echo "expected '$edge_action' to fail on the $first edge, got: $out"
  exit 1
fi
accepts output-focus-next
wait_for_focused_output "$other"
accepts output-focus-next
wait_for_focused_output "$first"

# With two outputs the reverse cycle reaches the same monitor, so one keybind
# per direction is enough.
accepts output-focus-previous
wait_for_focused_output "$other"
accepts output-focus-previous
wait_for_focused_output "$first"

# A window follows the same cycle, wrap included.
spawn_client traveller
wait_for_window_output traveller "$first"
accepts window-move-to-output-next
wait_for_window_output traveller "$other"
wait_for_focused_output "$other"
accepts window-move-to-output-next
wait_for_window_output traveller "$first"

echo "output-focus and window-move-to-output cycled across both monitors and wrapped at the end"
