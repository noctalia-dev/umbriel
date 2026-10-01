#!/usr/bin/env bash
# Whole-scene authored pair, independent frozen source and live destination.
set -euo pipefail
trap 'echo "pair assertion at line $LINENO"; "$UMBRIEL" effects --json' ERR
cat > "$UMBRIEL_RUNTIME_DIR/melt.glsl" <<'SHADER'
vec4 transition(vec2 uv) {
  return mix(umbriel_sample_from(uv), umbriel_sample_to(uv), umbriel_progress);
}
SHADER
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[output.HEADLESS-1]
mode = "640x360"
workspaces = 3
[animation]
enabled = true
duration_ms = 1000
curve = "linear"
[animation.windows_in]
enabled = false
[animation.windows_move]
enabled = false
[animation.workspaces]
effect = "melt"
[keybinds]
"Mod+1" = "workspace-switch:1"
"Mod+3" = "workspace-switch:3"
"Mod+WheelDown" = "workspace-next"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
[effects.preset.melt]
kind = "animation"
interface = "scene-v1"
scope = "workspace_pair"
shader = "melt.glsl"
[[window_rule]]
match.title = "^pair-from$"
default_workspace = 1
default_focused = false
[[window_rule]]
match.title = "^pair-to$"
default_workspace = 3
default_focused = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
mkfifo "$UMBRIEL_RUNTIME_DIR/from-input" "$UMBRIEL_RUNTIME_DIR/to-input"
exec 7<> "$UMBRIEL_RUNTIME_DIR/from-input"
exec 8<> "$UMBRIEL_RUNTIME_DIR/to-input"
SOURCE_UPDATES=1 "$UMBRIEL_SEAT_LOG_CLIENT" pair-from <&7 > "$UMBRIEL_RUNTIME_DIR/from.log" 2>&1 &
from=$!
SOURCE_UPDATES=1 "$UMBRIEL_SEAT_LOG_CLIENT" pair-to <&8 > "$UMBRIEL_RUNTIME_DIR/to.log" 2>&1 &
to=$!
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq length) == 2 ]] && break
  sleep .02
done
"$UMBRIEL" settle
"$UMBRIEL" clock-freeze
state() {
  "$UMBRIEL" effects --json | jq '.owners[] | select(.type == "output" and .name == "HEADLESS-1") | .workspace_transition'
}
ready() {
  for _ in $(seq 100); do
    [[ $(state | jq .source_ready) == true ]] && return 0
    sleep .02
  done
  state
  return 1
}
grim "$UMBRIEL_RUNTIME_DIR/from.png"
"$UMBRIEL_POINTER_CLIENT" 640 360 move 320 180 mod logo pause 100 tap 4 mod none
ready
state | jq -e '.active and .source_ready and .progress == 0 and .memory_bytes > 0' > /dev/null
grim "$UMBRIEL_RUNTIME_DIR/start.png"
cmp "$UMBRIEL_RUNTIME_DIR/from.png" "$UMBRIEL_RUNTIME_DIR/start.png"
# Outgoing client updates after acquisition must not change retained source.
printf 'n\n' >&7
sleep .08
grim "$UMBRIEL_RUNTIME_DIR/frozen.png"
cmp "$UMBRIEL_RUNTIME_DIR/start.png" "$UMBRIEL_RUNTIME_DIR/frozen.png"
# Destination updates remain live throughout the transition.
printf 'n\n' >&8
sleep .08
"$UMBRIEL" clock-advance 500
grim "$UMBRIEL_RUNTIME_DIR/mid.png"
# Both samples must blend full images at resting positions, without native slide offsets.
for point in '240 90' '400 270'; do
  read -r x y <<< "$point"
  read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/mid.png" pixel "$x" "$y")
  (( r >= 23 && r <= 28 && g >= 168 && g <= 172 && b >= 125 && b <= 130 )) || {
    echo "incorrect resting-position blend: $x $y: $r $g $b"; exit 1;
  }
done
state | jq -e '.active and .progress > .4 and .progress < .6' > /dev/null
"$UMBRIEL" clock-advance 1500
"$UMBRIEL" settle
state | jq -e '(.active | not) and .memory_bytes == 0' > /dev/null
"$UMBRIEL" workspaces --json | jq -e '.[2].active' > /dev/null
grim "$UMBRIEL_RUNTIME_DIR/end.png"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/end.png" pixel 320 180)
(( g > 170 && r < 30 && b < 80 )) || { echo "destination did not remain live: $r $g $b"; exit 1; }
# Nonadjacent retarget ends at the native destination.
"$UMBRIEL_POINTER_CLIENT" 640 360 mod logo tap 2 mod none
ready
"$UMBRIEL" clock-advance 250
"$UMBRIEL" msg workspace-switch:2
ready
"$UMBRIEL" clock-advance 1500
"$UMBRIEL" settle
state | jq -e '(.active | not) and .memory_bytes == 0' > /dev/null
"$UMBRIEL" workspaces --json | jq -e '.[1].active' > /dev/null
# Wheel bindings enter exactly the same native transition lifecycle.
"$UMBRIEL_POINTER_CLIENT" 640 360 mod logo notch 1 mod none
ready
"$UMBRIEL" workspaces --json | jq -e '.[2].active' > /dev/null
"$UMBRIEL" clock-advance 1500
"$UMBRIEL" settle
state | jq -e '(.active | not) and .memory_bytes == 0' > /dev/null
kill "$from" "$to"
echo 'Workspace pair preserves frozen source, live destination, resting-position blend and native transition cleanup'
