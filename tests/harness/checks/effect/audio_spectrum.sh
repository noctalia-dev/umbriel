#!/usr/bin/env bash
# The shipped spectrum must visibly respond to quiet music-level inputs, without
# a shader clock, and return to a quiet native border at zero.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/spectrum.png"
readonly HELPER="$(realpath "$(dirname "$UMBRIEL_UNMAP_CLIENT")/audio-synthetic")"
cp "${UMBRIEL_SPECTRUM_SHADER:-$UMBRIEL_REPO/examples/effects/border/spectrum/shader.glsl}" "$UMBRIEL_RUNTIME_DIR/spectrum.glsl"
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
speed = 0
padding = 24
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
halo() { "$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$((x+w/2))" "$((y-12))"; }
inject 0 0
read -r zero_r zero_g zero_b < <(halo)
(( zero_r == 0 && zero_g == 0 && zero_b == 0 )) || { echo "silent spectrum added a halo"; exit 1; }
inject 0.02 0
read -r level_r level_g level_b < <(halo)
(( level_b >= 12 )) || { echo "quiet RMS is invisible in a low-energy frequency band: $level_r $level_g $level_b"; exit 1; }
inject 0.02 0.006
read -r quiet_r quiet_g quiet_b < <(halo)
(( quiet_b >= level_b + 8 && quiet_g >= 6 )) || { echo "quiet playback spectrum is imperceptible: $quiet_r $quiet_g $quiet_b"; exit 1; }
cp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/quiet.png"
"$UMBRIEL" settle > /dev/null
grim -s 1 -o HEADLESS-1 "$IMAGE"
cmp "$IMAGE" "$UMBRIEL_RUNTIME_DIR/quiet.png" || { echo "held audio changed without time or a new snapshot"; exit 1; }
inject 0.02 0.025
read -r stronger_r stronger_g stronger_b < <(halo)
(( stronger_b > quiet_b + 15 )) || { echo "band change did not visibly expand spectrum halo: $quiet_b -> $stronger_b"; exit 1; }
inject 0 0
read -r end_r end_g end_b < <(halo)
(( end_r == 0 && end_g == 0 && end_b == 0 )) || { echo "spectrum did not return to zero"; exit 1; }
echo "shipped spectrum responds to quiet playback bands, holds without a clock, and returns to zero"
