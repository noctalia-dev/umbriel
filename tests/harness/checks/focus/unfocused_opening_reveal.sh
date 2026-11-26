#!/usr/bin/env bash
# An opener that keeps focus elsewhere must not scroll the strip to its own fullscreen or edge-maximized column.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/unfocused-opening-reveal.png"

windows() { "$UMBRIEL" windows --json; }

wait_for_window() {
  local title=$1
  for _ in $(seq 80); do
    if windows | jq -e --arg title "$title" 'any(.[]; .title == $title)' >/dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "window $title did not map: $(windows)"
  return 1
}

wait_for_absent() {
  local title=$1
  for _ in $(seq 80); do
    if windows | jq -e --arg title "$title" 'all(.[]; .title != $title)' >/dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "window $title did not unmap: $(windows)"
  return 1
}

field_of() {
  windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

assert_active() {
  local title=$1
  if ! windows | jq -e --arg title "$title" 'any(.[]; .title == $title and .active)' >/dev/null; then
    echo "expected $title to own keyboard focus: $(windows)"
    return 1
  fi
}

assert_pixel() {
  local expected=$1 context=$2 actual
  actual=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel 300 360)
  if [[ $actual != "$expected" ]]; then
    echo "$context: expected RGB $expected at 300,360, got $actual"
    return 1
  fi
}

spawn_client() {
  local title=$1 color=$2
  env FOLLOW_CONFIGURES=1 FILL_COLOR="$color" "$CLIENT" "$title" 400 300 >"$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  CLIENT_PID=$!
  wait_for_window "$title"
}

stop_client() {
  kill -KILL "$CLIENT_PID" 2>/dev/null || true
  wait "$CLIENT_PID" 2>/dev/null || true
  wait_for_absent "$1"
}

cat >>"$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false

[layout.scrolling]
default_extent_fraction = 0.5
center_focused = "never"
center_underfull_strip = false

[[window_rule]]
match.title = "^reveal-edges$"
default_maximize_to_edges = true
default_focused = false

[[window_rule]]
match.title = "^reveal-fullscreen$"
default_fullscreen = true
default_focused = false
EOF
"$UMBRIEL" msg config-reload >/dev/null

spawn_client reveal-focus 0xFF0000FF
"$UMBRIEL" settle
assert_active reveal-focus
focused_x=$(field_of reveal-focus x)

spawn_client reveal-edges 0xFF00FF00
"$UMBRIEL" settle
assert_active reveal-focus
if [[ $(field_of reveal-edges maximized_to_edges) != true ]]; then
  echo "the unfocused opener did not open maximized to edges: $(windows)"
  exit 1
fi
if [[ $(field_of reveal-focus x) != "$focused_x" ]]; then
  echo "an unfocused edge-maximized opener scrolled the strip: $(windows)"
  exit 1
fi
grim -o HEADLESS-1 "$SHOT"
assert_pixel '0 0 255' "an unfocused edge-maximized opener covered the focused window"
stop_client reveal-edges
"$UMBRIEL" settle
assert_active reveal-focus
focused_x=$(field_of reveal-focus x)

spawn_client reveal-fullscreen 0xFF00FF00
"$UMBRIEL" settle
assert_active reveal-focus
if [[ $(field_of reveal-fullscreen fullscreen) != true ]]; then
  echo "the unfocused opener did not open fullscreen: $(windows)"
  exit 1
fi
if [[ $(field_of reveal-focus x) != "$focused_x" ]]; then
  echo "an unfocused fullscreen opener scrolled the strip: $(windows)"
  exit 1
fi
grim -o HEADLESS-1 "$SHOT"
assert_pixel '0 0 255' "an unfocused fullscreen opener covered the focused window"

echo "unfocused fullscreen and edge-maximized openers kept the focused scrolling column revealed"
