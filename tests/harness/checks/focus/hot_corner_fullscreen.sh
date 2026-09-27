#!/usr/bin/env bash
# A focused fullscreen window blocks a hot corner. Moving focus to another window must restore the corner even while
# the first window retains its fullscreen state on the same workspace.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

wait_for_windows() {
  local query=$1 message=$2 windows=
  for _ in $(seq 60); do
    windows=$("$UMBRIEL" windows --json)
    jq -e "$query" <<< "$windows" > /dev/null && return 0
    sleep 0.1
  done
  echo "$message: $windows"
  return 1
}

wait_for_fullscreen() {
  local state=
  for _ in $(seq 60); do
    state=$("$UMBRIEL" tearing --json)
    if jq -e '[.surfaces[] | select(.title == "hot-corner-fullscreen" and .fullscreen)] | length == 1' \
      <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "first window did not enter fullscreen: $state"
  return 1
}

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r \
    '.[] | select(.output == "HEADLESS-1" and .active) | .index'
}

wait_for_workspace() {
  local want=$1 actual=
  for _ in $(seq 40); do
    actual=$(active_workspace)
    [[ $actual == "$want" ]] && return 0
    sleep 0.05
  done
  echo "hot corner did not switch to workspace $want, active workspace is ${actual:-missing}"
  return 1
}

# Keep the pointer away from the corner while its action becomes live.
pointer move 640 360
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output.HEADLESS-1]
workspaces = 2

[animation]
enabled = false

[input.focus]
follows_mouse = false

[hot_corners.top_left]
enabled = true
delay_ms = 100
action = "workspace-switch:2/HEADLESS-1"
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" hot-corner-fullscreen > "$UMBRIEL_RUNTIME_DIR/hot-corner-fullscreen.log" 2>&1 &
wait_for_windows \
  '[.[] | select(.title == "hot-corner-fullscreen" and .focused and .active)] | length == 1' \
  "fullscreen test window did not map with focus"
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen

# The fullscreen window owns focus, so dwelling in the corner must do nothing.
pointer move 0 0 pause 250
if [[ $(active_workspace) != 1 ]]; then
  echo "covered hot corner fired over fullscreen content"
  exit 1
fi
pointer move 640 360

"$CLIENT" hot-corner-focused > "$UMBRIEL_RUNTIME_DIR/hot-corner-focused.log" 2>&1 &
wait_for_windows \
  'length == 2 and ([.[] | select(.title == "hot-corner-focused")] | length == 1)' \
  "second test window did not map"
focused_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "hot-corner-focused") | .id')
"$UMBRIEL" msg "window-focus:$focused_id" > /dev/null
wait_for_windows \
  '([.[] | select(.title == "hot-corner-fullscreen") | [.focused, .active]] == [[false, false]])
    and ([.[] | select(.title == "hot-corner-focused") | [.focused, .active]] == [[true, true]])' \
  "non-fullscreen window did not take focus"
wait_for_fullscreen

# The fullscreen window has lost focus. Its retained fullscreen flag must not block the corner.
pointer move 0 0 pause 250
wait_for_workspace 2

echo "only the focused fullscreen window suppresses hot corners"
