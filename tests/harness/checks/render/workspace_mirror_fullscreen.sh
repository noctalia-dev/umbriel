#!/usr/bin/env bash
# harness: outputs=2
# A copied fullscreen surface must cover top-layer panels just like its original, and uncover them on exit.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[animation]
enabled = false
[output.HEADLESS-1]
mode = "1280x720@60"
position = [0, 0]
workspaces = ["source", "away"]
[output.HEADLESS-2]
mode = "960x540@60"
position = [1280, 0]
workspaces = ["own", "source"]
[[workspace]]
output = "HEADLESS-2"
name = "source"
mirror = { output = "HEADLESS-1", name = "source" }
[[window_rule]]
match.title = "mirror-fullscreen"
default_output = "HEADLESS-1"
default_workspace = "source"
TOML
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL_SEAT_LOG_CLIENT" mirror-fullscreen > "$UMBRIEL_RUNTIME_DIR/window.log" 2>&1 &
window_pid=$!
await_events "$UMBRIEL_RUNTIME_DIR/window.log" keyboard-enter 1
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 > "$UMBRIEL_RUNTIME_DIR/panel-source.log" 2>&1 &
window_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "mirror-fullscreen") | .id')
"$UMBRIEL_LAYER_CLIENT" HEADLESS-2 40 > "$UMBRIEL_RUNTIME_DIR/panel-copy.log" 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" layers --json | jq '[.[] | select(.mapped)] | length') == 2 ]] && break
  sleep 0.05
done
[[ $("$UMBRIEL" layers --json | jq '[.[] | select(.mapped)] | length') == 2 ]]

assert_panel_pixel() {
  local output=$1 expected=$2 label=$3 actual
  "$UMBRIEL" settle > /dev/null
  grim -o "$output" "$UMBRIEL_RUNTIME_DIR/panel.png"
  actual=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/panel.png" pixel 100 20)
  if [[ $actual != "$expected" ]]; then
    echo "$label: expected $expected on $output, got $actual"
    return 1
  fi
}

"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
assert_panel_pixel HEADLESS-2 "32 32 32" "normal copy leaves its panel visible"
"$UMBRIEL" msg output-focus-left > /dev/null
"$UMBRIEL" msg window-focus "$window_id" > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
assert_panel_pixel HEADLESS-1 "51 136 204" "fullscreen original covers its panel"
assert_panel_pixel HEADLESS-2 "51 136 204" "fullscreen copy covers its panel"

# Menus and overlays retain their priority above fullscreen; only the top-layer bar is covered.
"$UMBRIEL_LAYER_CLIENT" HEADLESS-2 0 overlay-layer > "$UMBRIEL_RUNTIME_DIR/overlay.log" 2>&1 &
overlay_pid=$!
await_lines "$UMBRIEL_RUNTIME_DIR/overlay.log" ready 1
assert_panel_pixel HEADLESS-2 "255 0 0" "overlay remains above the fullscreen copy"
kill "$overlay_pid"
wait "$overlay_pid" || true
assert_panel_pixel HEADLESS-2 "51 136 204" "closing overlay reveals the fullscreen copy"

"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
assert_panel_pixel HEADLESS-1 "32 32 32" "origin away workspace restores its panel"
assert_panel_pixel HEADLESS-2 "51 136 204" "hidden origin keeps the copy above its panel"
"$UMBRIEL" msg workspace-switch source --output HEADLESS-1 > /dev/null
"$UMBRIEL" msg window-focus "$window_id" > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
assert_panel_pixel HEADLESS-2 "32 32 32" "fullscreen exit restores the copy panel"

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
assert_panel_pixel HEADLESS-2 "51 136 204" "fullscreen re-entry covers the copy panel"
"$UMBRIEL" msg workspace-switch own --output HEADLESS-2 > /dev/null
assert_panel_pixel HEADLESS-2 "32 32 32" "leaving the copy restores its panel"
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
assert_panel_pixel HEADLESS-2 "51 136 204" "returning to the copy covers its panel"
kill "$window_pid"
wait "$window_pid" || true
assert_panel_pixel HEADLESS-2 "32 32 32" "closing the fullscreen window restores the panel"
echo 'Copied fullscreen covers top-layer panels with a hidden origin; exit, workspace switch and close restore them.'
