#!/usr/bin/env bash
# Both clients must repaint after frame callbacks while reveal progress is held.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/reveal.png"
cp "$UMBRIEL_REPO/examples/effects/animation/workspace_wipe/shader.glsl" "$UMBRIEL_RUNTIME_DIR/wipe.glsl"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = true
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
[animation.windows_move]
enabled = false
[animation.workspaces]
style = "reveal"
effect = "wipe"
duration_ms = 1000
curve = "linear"
[effects.preset.wipe]
kind = "animation"
shader = "wipe.glsl"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[[window_rule]]
match.title = "^reveal-"
default_floating = true
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
mkfifo "$UMBRIEL_RUNTIME_DIR/a.fifo" "$UMBRIEL_RUNTIME_DIR/b.fifo"
exec {a_fd}<>"$UMBRIEL_RUNTIME_DIR/a.fifo"
exec {b_fd}<>"$UMBRIEL_RUNTIME_DIR/b.fifo"
REPAINT_ON_STDIN=1 FILL_COLOR=0xFFFF0000 "$UMBRIEL_UNMAP_CLIENT" reveal-a 600 500 \
  <&"$a_fd" > "$UMBRIEL_RUNTIME_DIR/a.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/a.log" mapped 1
"$UMBRIEL" settle
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" settle
REPAINT_ON_STDIN=1 FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" reveal-b 400 600 \
  <&"$b_fd" > "$UMBRIEL_RUNTIME_DIR/b.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/b.log" mapped 1
"$UMBRIEL" settle
"$UMBRIEL" msg workspace-switch:1 > /dev/null
"$UMBRIEL" settle

pixel() { "$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel "$1" "$2"; }
expect_pixel() {
  local x=$1 y=$2 want=$3 value
  value=$(pixel "$x" "$y")
  [[ $value == "$want" ]] || { echo "pixel $x,$y: expected $want, got $value"; return 1; }
}
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" clock-advance 500
# Default vertical axis: incoming at the top, outgoing at the bottom. Both
# floating clients cover these positions at rest; a half slide would not.
grim "$SHOT"
expect_pixel 640 200 '0 0 255'
expect_pixel 640 520 '255 0 0'

# Reject a failed reveal frame and keep native navigation at the held progress.
"$UMBRIEL" animation-capture-fail on
"$UMBRIEL" clock-advance 1
grim "$SHOT"
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '0 0 255'
[[ $("$UMBRIEL" windows --json | jq -r '.[] | select(.active) | .title') == reveal-b ]]
"$UMBRIEL" animation-capture-fail off
"$UMBRIEL" clock-advance 1
grim "$SHOT"
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '0 0 255'
# The next native transition may reveal again.
"$UMBRIEL" clock-advance 1000
"$UMBRIEL" msg workspace-switch:1 > /dev/null
"$UMBRIEL" clock-advance 1000
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" clock-advance 500
grim "$SHOT"
expect_pixel 640 200 '0 0 255'
expect_pixel 640 520 '255 0 0'

printf G >&"$a_fd"
await_lines "$UMBRIEL_RUNTIME_DIR/a.log" repainted 1
for _ in $(seq 60); do
  grim "$SHOT"
  [[ $(pixel 640 520) == '0 255 0' ]] && break
  sleep 0.025
done
expect_pixel 640 520 '0 255 0'
expect_pixel 640 200 '0 0 255'
printf R >&"$b_fd"
await_lines "$UMBRIEL_RUNTIME_DIR/b.log" repainted 1
for _ in $(seq 60); do
  grim "$SHOT"
  [[ $(pixel 640 200) == '255 0 0' ]] && break
  sleep 0.025
done
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '0 255 0'

# The outgoing view is visual only even where its revealed pixels remain.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 520 click 272
"$UMBRIEL" workspaces --json | jq -e '.[] | select(.active) | .name == "2"' > /dev/null
"$UMBRIEL" clock-advance 500
grim "$SHOT"
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '255 0 0'

# Reverse direction and interrupt at half progress with a native rebase.
"$UMBRIEL" msg workspace-switch:1 > /dev/null
"$UMBRIEL" clock-advance 500
grim "$SHOT"
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '0 255 0'
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" clock-advance 1000
grim "$SHOT"
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '255 0 0'
# Fullscreen uses a separate scene root but must share the same reveal edge.
"$UMBRIEL" clock-resume
"$UMBRIEL" settle
b_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "reveal-b") | .id')
"$UMBRIEL" msg "window-focus-warp:$b_id" > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg workspace-switch:1 > /dev/null
"$UMBRIEL" clock-advance 500
grim "$SHOT"
expect_pixel 640 200 '255 0 0'
expect_pixel 640 520 '0 255 0'
# Disabling the event at held progress removes bindings and finishes at native active workspace 1.
sed -i '/^\[animation.workspaces\]$/a enabled = false' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" clock-advance 1
grim "$SHOT"
expect_pixel 640 200 '0 255 0'
expect_pixel 640 520 '0 255 0'
"$UMBRIEL" clock-resume
"$UMBRIEL" settle

# Horizontal reveal and renderer replacement at held progress.
sed -i '/^\[animation.workspaces\]$/{n; /enabled = false/d;}' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[output."HEADLESS-1"]
workspace_axis = "horizontal"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" clock-advance 500
grim "$SHOT"
expect_pixel 500 360 '255 0 0'
expect_pixel 780 360 '0 255 0'
"$UMBRIEL" renderer-recover > /dev/null
await_lines "$UMBRIEL_LOG" '.*renderer recreated' 1
# Supply a fresh buffer after renderer replacement.
printf R >&"$b_fd"
await_lines "$UMBRIEL_RUNTIME_DIR/b.log" repainted 2
"$UMBRIEL" clock-advance 1
grim "$SHOT"
expect_pixel 500 360 '255 0 0'
expect_pixel 780 360 '255 0 0'
# A new reveal uses the replacement renderer; an empty endpoint leaves no source pixels.
"$UMBRIEL" msg workspace-switch:3 > /dev/null
"$UMBRIEL" clock-advance 1000
grim "$SHOT"
[[ $(pixel 640 360) != '255 0 0' ]]
"$UMBRIEL" clock-resume
"$UMBRIEL" settle

echo 'live frame-paced clients, resting presentation, different bounds, fullscreen, reverse direction, native rebase, focus and disable cleanup verified'
