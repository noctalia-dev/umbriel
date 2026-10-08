#!/usr/bin/env bash
# Sparse requests and two sessions on one source must return fresh unfiltered pixels.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
cat > "$UMBRIEL_RUNTIME_DIR/blue.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(0.0, 0.0, 1.0, 1.0); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[effects]
in_capture = false
[effects.preset.blue]
kind = "window"
shader = "blue.glsl"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
[[window_rule]]
match.title = "^capture-demand$"
default_floating = true
default_position = { x = 100, y = 60, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
mkfifo "$UMBRIEL_RUNTIME_DIR/repaint.fifo" "$UMBRIEL_RUNTIME_DIR/capture.fifo"
exec 8<>"$UMBRIEL_RUNTIME_DIR/repaint.fifo"
exec 9<>"$UMBRIEL_RUNTIME_DIR/capture.fifo"
REPAINT_ON_STDIN=1 FILL_COLOR=0xFFFF0000 "$UMBRIEL_UNMAP_CLIENT" capture-demand 1000 600 \
  < "$UMBRIEL_RUNTIME_DIR/repaint.fifo" > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" mapped 1
"$UMBRIEL" settle
id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "capture-demand") | .id')
"$UMBRIEL" msg "effect-window-set:blue/$id" >/dev/null
"$UMBRIEL" settle
"$UMBRIEL_CAPTURE_CLIENT" --manual --shared-source --output HEADLESS-1 --pixel-output HEADLESS-1 \
  < "$UMBRIEL_RUNTIME_DIR/capture.fifo" > "$UMBRIEL_RUNTIME_DIR/capture.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/capture.log" manual-ready 1
commands=0
command_capture() {
  printf '%s\n' "$1" >&9
  commands=$((commands + 1))
  await_lines "$UMBRIEL_RUNTIME_DIR/capture.log" manual-done "$commands"
}
command_capture 'capture primary'
rg -q '^pixel primary 255 0 0$' "$UMBRIEL_RUNTIME_DIR/capture.log"
# Neither session has a pending request while actual client commits change pixels.
printf G >&8
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" repainted 1
"$UMBRIEL" settle
# A concurrent screencopy consumer must still receive fresh unfiltered pixels.
grim "$UMBRIEL_RUNTIME_DIR/green.png"
[[ $("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/green.png" pixel 640 360) == '0 255 0' ]]
command_capture 'capture primary'
rg -q '^pixel primary 0 255 0$' "$UMBRIEL_RUNTIME_DIR/capture.log"
command_capture 'capture pixel'
rg -q '^pixel pixel 0 255 0$' "$UMBRIEL_RUNTIME_DIR/capture.log"
# Destroying one session must not discount the surviving source lock twice.
command_capture 'destroy primary'
printf R >&8
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" repainted 2
"$UMBRIEL" settle
command_capture 'capture pixel'
rg -q '^pixel pixel 255 0 0$' "$UMBRIEL_RUNTIME_DIR/capture.log"
command_capture 'destroy pixel'
"$UMBRIEL" settle
# Releasing the final session must not leave stale capture pixels behind.
printf G >&8
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" repainted 3
"$UMBRIEL" settle
grim "$UMBRIEL_RUNTIME_DIR/released.png"
[[ $("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/released.png" pixel 640 360) == '0 255 0' ]]
echo 'sparse fresh captures, shared-source survival, concurrent screencopy and final teardown verified'
