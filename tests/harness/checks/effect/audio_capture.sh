#!/usr/bin/env bash
# A hidden view's active isolated capture owns audio demand and its own latch.
# Frozen injection changes captured pixels without requesting display frames.
set -euo pipefail
readonly HELPER="$(realpath "$(dirname "$UMBRIEL_UNMAP_CLIENT")/audio-synthetic")"
readonly CAPTURE="$(dirname "$UMBRIEL_UNMAP_CLIENT")/toplevel-capture-client"
readonly LOG="$UMBRIEL_RUNTIME_DIR/audio-capture.jsonl"
cat > "$UMBRIEL_RUNTIME_DIR/audio-capture.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(0.0, umbriel_audio_rms(), 0.0, 1.0); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[effects]
window = "capture-audio"
in_capture = true
max_fps = 12
[effects.audio.sources.desktop]
provider = "external"
mode = "playback"
target = "explicit-test-source"
executable = "$HELPER"
args = ["--external-test", "--silence"]
[effects.preset.capture-audio]
kind = "window"
shader = "audio-capture.glsl"
audio = "desktop"
[[window_rule]]
match.title = "^audio-capture-only$"
default_floating = true
EOF
"$UMBRIEL" msg config-reload > /dev/null
# The explicit reload and the config watcher's queued reload are independent.
# Neither may remain in flight when the no-native-commit observation begins.
for _ in $(seq 100); do
  (( $(grep -c 'config reloaded' "$UMBRIEL_RUNTIME_DIR/compositor.log") >= 2 )) && break
  sleep 0.02
done
(( $(grep -c 'config reloaded' "$UMBRIEL_RUNTIME_DIR/compositor.log") >= 2 ))
FILL_COLOR=0xFF000000 "$UMBRIEL_UNMAP_CLIENT" audio-capture-only 300 200 > "$UMBRIEL_RUNTIME_DIR/audio-client.log" 2>&1 &
for _ in $(seq 80); do
  id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "audio-capture-only") | .id')
  [[ -n $id ]] && break
  sleep 0.025
done
[[ -n $id ]]
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" effects --json | jq -e '.audio_demanded_sources == 0' > /dev/null
"$UMBRIEL" clock-freeze > /dev/null
# Clock control explicitly schedules every native output; drain that request
# before attributing any later buffer commit to isolated capture work.
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" effect-frames --json | jq -e 'all(.outputs[]; .buffer_commits | type == "number")' > /dev/null
before=$("$UMBRIEL" effect-frames --json | jq '[.outputs[] | [.effect_frames, .buffer_commits]]')
"$CAPTURE" "$id" 1000 > "$LOG" 2> "$UMBRIEL_RUNTIME_DIR/audio-capture-client.log" &
capture_pid=$!
for _ in $(seq 100); do
  "$UMBRIEL" effects --json | jq -e '.audio_demanded_sources == 1 and (.audio[] | select(.name == "desktop") | .available)' > /dev/null && break
  sleep 0.02
done
"$UMBRIEL" effects --json | jq -e '.audio_demanded_sources == 1 and (.audio[] | select(.name == "desktop") | .available)' > /dev/null
inject() {
  "$UMBRIEL" audio-inject "$(jq -nc --argjson v "$1" '{source:"desktop",rms:$v,peak:$v,envelope:$v,bands:[range(16)|$v]}')" > /dev/null
}
inject 0.8
for _ in $(seq 100); do
  jq -se 'any(.[]; .frame != null and .g > 180 and .r < 20 and .b < 20)' "$LOG" > /dev/null && break
  sleep 0.02
done
jq -se 'any(.[]; .frame != null and .g > 180 and .r < 20 and .b < 20)' "$LOG" > /dev/null
last=$(jq -sr '[.[] | select(.frame != null)][-1].frame' "$LOG")
inject 0
for _ in $(seq 100); do
  jq -se --argjson last "$last" 'any(.[]; .frame > $last and .r < 20 and .g < 20 and .b < 20)' "$LOG" > /dev/null && break
  sleep 0.02
done
jq -se --argjson last "$last" 'any(.[]; .frame > $last and .r < 20 and .g < 20 and .b < 20)' "$LOG" > /dev/null
inject 0.6
for _ in $(seq 100); do
  jq -se --argjson last "$last" 'any(.[]; .frame > $last and .g > 130)' "$LOG" > /dev/null && break
  sleep 0.02
done
jq -se --argjson last "$last" 'any(.[]; .frame > $last and .g > 130)' "$LOG" > /dev/null
after=$("$UMBRIEL" effect-frames --json | jq '[.outputs[] | [.effect_frames, .buffer_commits]]')
[[ $before == "$after" ]] || { echo "capture-only audio woke a display: $before -> $after"; exit 1; }
last=$(jq -sr '[.[] | select(.frame != null)][-1].frame' "$LOG")
"$UMBRIEL" clock-resume > /dev/null
for _ in $(seq 100); do
  jq -se --argjson last "$last" 'any(.[]; .frame > $last and .g < 20)' "$LOG" > /dev/null && break
  sleep 0.02
done
jq -se --argjson last "$last" 'any(.[]; .frame > $last and .g < 20)' "$LOG" > /dev/null
# Resume also schedules a native frame independently of capture. Release must
# remain quiet after that explicit request has completed.
"$UMBRIEL" settle > /dev/null
before=$("$UMBRIEL" effect-frames --json | jq '[.outputs[] | [.effect_frames, .buffer_commits]]')
kill "$capture_pid"
wait "$capture_pid" || true
for _ in $(seq 100); do
  "$UMBRIEL" effects --json | jq -e '.audio_demanded_sources == 0' > /dev/null && break
  sleep 0.02
done
"$UMBRIEL" effects --json | jq -e '.audio_demanded_sources == 0' > /dev/null
after=$("$UMBRIEL" effect-frames --json | jq '[.outputs[] | [.effect_frames, .buffer_commits]]')
[[ $before == "$after" ]] || { echo "capture release woke a display: $before -> $after"; exit 1; }
echo "hidden capture-only audio changed pixels, zero control passed, displays stayed idle, and capture release stopped demand"
