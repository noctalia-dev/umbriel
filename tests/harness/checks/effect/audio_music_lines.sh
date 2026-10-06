#!/usr/bin/env bash
# Fixed spectrum bars and contours: spectral locality, no time-driven motion,
# quiet/strong audio response, unchanged window centre and silent recovery.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/spectrum.png"
readonly HELPER="$(realpath "$(dirname "$UMBRIEL_UNMAP_CLIENT")/audio-synthetic")"
cp "$UMBRIEL_REPO/examples/effects/border/music-lines/shader.glsl" "$UMBRIEL_RUNTIME_DIR/spectrum.glsl"
cp "$UMBRIEL_REPO/examples/effects/border/music-lines/overlay.glsl" "$UMBRIEL_RUNTIME_DIR/overlay.glsl"
cat >> "$UMBRIEL_CONFIG" <<EOF
[animation]
enabled = false
[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
[effects]
border = "spectrum"
in_capture = true
[effects.audio.sources.desktop]
provider = "external"
mode = "playback"
target = "explicit-test-source"
executable = "$HELPER"
args = ["--external-test", "--silence"]
[effects.preset.spectrum]
kind = "border"
shader = "spectrum.glsl"
audio = "desktop"
animated = false
padding = 48
overlay = "music-lines-overlay"
[effects.preset.music-lines-overlay]
kind = "window"
shader = "overlay.glsl"
audio = "desktop"
[[window_rule]]
match.title = "^spectrum-fixture$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFF000000 "$UMBRIEL_UNMAP_CLIENT" spectrum-fixture 300 200 > "$UMBRIEL_RUNTIME_DIR/spectrum-client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "spectrum-fixture")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
read -r x y w < <(jq -r '"\(.x) \(.y) \(.w)"' <<< "$window")
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" clock-freeze > /dev/null
inject() {
  "$UMBRIEL" audio-inject "$(jq -nc --argjson level "$1" --argjson band "$2" '{source:"desktop",rms:$level,peak:$level,envelope:$level,bands:[range(16)|$band]}')" > /dev/null
  "$UMBRIEL" settle > /dev/null
  grim -s 1 -o HEADLESS-1 "$IMAGE"
}
readonly OUTSIDE="200x42+$((x+50))+$((y-48))"
readonly INSIDE="200x28+$((x+50))+$((y+1))"
coloured() { "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r>.04||g>.04||b>.04' "$1"; }
inject 0 0
cp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/silent.png"
(( $(coloured "$OUTSIDE") == 0 && $(coloured "$INSIDE") == 0 ))
# Selecting this effect removes the native static ring, even at zero input.
(( $(coloured "200x4+$((x+50))+$((y-4))") == 0 )) || { echo "static border survived"; exit 1; }
inject .02 .006
outer=$(coloured "$OUTSIDE"); inner=$(coloured "$INSIDE")
(( outer>100 && outer<6000 && inner>80 && inner<4000 )) || { echo "waveform coverage missing or opaque: $outer $inner"; exit 1; }
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$((x+w/2))" "$((y+100))")
(( r==0 && g==0 && b==0 )) || { echo "paired overlay obscures window centre"; exit 1; }
cp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/quiet.png"
quiet_outer=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" mean "$OUTSIDE")
quiet_inner=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" mean "$INSIDE")
"$UMBRIEL" settle > /dev/null
grim -s 1 -o HEADLESS-1 "$IMAGE"
cmp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/quiet.png"
inject .06 .025
strong_outer=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" mean "$OUTSIDE")
strong_inner=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" mean "$INSIDE")
python3 - "$quiet_outer" "$strong_outer" "$quiet_inner" "$strong_inner" <<'CHECK'
import sys
for quiet,strong in [(sys.argv[1],sys.argv[2]),(sys.argv[3],sys.argv[4])]:
    a,b=sum(map(float,quiet.split())),sum(map(float,strong.split()))
    assert b > a*1.2,(a,b)
CHECK
cp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/strong.png"
if [[ -n ${UMBRIEL_MUSIC_LINES_PREVIEW:-} ]]; then
  cp "$IMAGE" "$UMBRIEL_MUSIC_LINES_PREVIEW"
fi
"$UMBRIEL" clock-advance 230 > /dev/null
inject .06 .025
cmp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/strong.png" || { echo "held audio moved when only the clock changed"; exit 1; }
# Equal RMS but different spectral peaks must move only their fixed frequency
# regions. A clock-driven decorative waveform fails this locality contract.
peak() {
  "$UMBRIEL" audio-inject "$(jq -nc --argjson band "$1" '{source:"desktop",rms:0.02,peak:0.02,envelope:0.02,bands:[range(16)|if .==$band then 0.025 else 0.001 end]}')" > /dev/null
  "$UMBRIEL" settle > /dev/null
  grim -s 1 -o HEADLESS-1 "$IMAGE"
}
readonly BASS="20x28+$((x+50))+$((y-40))"
readonly TREBLE="20x28+$((x+230))+$((y-40))"
peak 3
(( $(coloured "$BASS") > 50 && $(coloured "$TREBLE") == 0 )) || { echo "bass peak did not stay in its fixed region"; exit 1; }
peak 12
(( $(coloured "$TREBLE") > 50 && $(coloured "$BASS") == 0 )) || { echo "treble peak did not stay in its fixed region"; exit 1; }
if [[ -n ${UMBRIEL_MUSIC_LINES_PREVIEW:-} ]]; then
  cp "$IMAGE" "$UMBRIEL_MUSIC_LINES_PREVIEW"
fi
"$UMBRIEL" effects --json | jq -e '.audio_demanded_sources == 1 and any(.owners[]; .slots.border.overlay? == "music-lines-overlay")' > /dev/null
inject 0 0
cmp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/silent.png"
echo "fixed spectrum bars respond to audio, isolate bass/treble regions, remain identical across clock advances, and disappear at silence"
