#!/usr/bin/env bash
# Real authored cursor/window presets: quiet audio visibility, stronger response,
# frozen stability, animated smoke/rings, source sharing and exact silent pixels.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/music.png"
readonly HELPER="$(realpath "$(dirname "$UMBRIEL_UNMAP_CLIENT")/audio-synthetic")"
cp "$UMBRIEL_REPO/examples/effects/cursor/music-radiance/shader.glsl" "$UMBRIEL_RUNTIME_DIR/radiance.glsl"
cp "$UMBRIEL_REPO/examples/effects/window/music-smoke/shader.glsl" "$UMBRIEL_RUNTIME_DIR/smoke.glsl"
cat >> "$UMBRIEL_CONFIG" <<EOF
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
[input.cursor]
hide_timeout_ms = 0
[effects]
window = "music-smoke"
cursor = "music-radiance"
in_capture = true
[effects.audio.sources.desktop]
provider = "external"
mode = "playback"
target = "explicit-test-source"
executable = "$HELPER"
args = ["--external-test", "--silence"]
[effects.preset.music-smoke]
kind = "window"
shader = "smoke.glsl"
audio = "desktop"
[effects.preset.music-radiance]
kind = "cursor"
shader = "radiance.glsl"
audio = "desktop"
radius = 160
[[window_rule]]
match.title = "^music-fixture$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFF000000 "$UMBRIEL_UNMAP_CLIENT" music-fixture 300 200 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "music-fixture")')
  [[ -n $window ]] && break
  sleep .025
done
[[ -n $window ]]
read -r x y w h < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
readonly WINDOW_REGION="$((w-20))x$((h-20))+$((x+10))+$((y+10))"
readonly CURSOR_REGION="240x240+680+180"
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 800 300 > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" clock-freeze > /dev/null
inject() {
  "$UMBRIEL" audio-inject "$(jq -nc --argjson level "$1" --argjson band "$2" '{source:"desktop",rms:$level,peak:$level,envelope:$level,bands:[range(16)|$band]}')" > /dev/null
  "$UMBRIEL" settle > /dev/null
  grim -s 1 -o HEADLESS-1 "$IMAGE"
}
coloured() { "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r>.04||g>.04||b>.04' "$1"; }
inject 0 0
(( $(coloured "$WINDOW_REGION") == 0 && $(coloured "$CURSOR_REGION") == 0 ))
cp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/silent.png"
inject .02 .006
(( $(coloured "$WINDOW_REGION") > 10000 && $(coloured "$CURSOR_REGION") > 1000 ))
cp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/quiet.png"
quiet_window=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" mean "$WINDOW_REGION")
quiet_cursor=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" mean "$CURSOR_REGION")
"$UMBRIEL" settle > /dev/null
grim -s 1 -o HEADLESS-1 "$IMAGE"
cmp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/quiet.png"
inject .06 .025
strong_window=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" mean "$WINDOW_REGION")
strong_cursor=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" mean "$CURSOR_REGION")
python3 - "$quiet_window" "$strong_window" "$quiet_cursor" "$strong_cursor" <<'PY'
import sys
for quiet, strong in [(sys.argv[1], sys.argv[2]), (sys.argv[3], sys.argv[4])]:
    a, b = sum(map(float, quiet.split())), sum(map(float, strong.split()))
    assert b > a * 1.15, (a, b)
PY
cp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/strong.png"
"$UMBRIEL" clock-advance 450 > /dev/null
inject .06 .025
if cmp -s "$IMAGE" "$UMBRIEL_RUNTIME_DIR/strong.png"; then
  echo "music effects did not animate"; exit 1
fi
"$UMBRIEL" effects --json | jq -e '.audio_demanded_sources == 1' > /dev/null
inject 0 0
cmp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/silent.png"
echo "music radiance and smoke respond to quiet/strong playback, animate, hold frozen inputs, share analysis and return to native silence"
