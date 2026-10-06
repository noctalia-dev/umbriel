#!/usr/bin/env bash
# harness: outputs=2
# A changing 60 Hz provider uses the existing effect cap, remains frozen through
# captures, and never requests effect frames on the independently idle output.
set -euo pipefail
helper=$(realpath "$(dirname "$UMBRIEL_UNMAP_CLIENT")/audio-synthetic")
cat > "$UMBRIEL_RUNTIME_DIR/audio-cadence.glsl" <<'GLSL'
vec4 screen(vec2 uv) { return vec4(umbriel_audio_rms(), 0.0, 0.0, 1.0); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
enabled = false
[effects]
screen = "audio-cadence"
in_capture = true
max_fps = 8
[effects.preset.audio-cadence]
kind = "screen"
shader = "audio-cadence.glsl"
audio = "fixture"
[effects.audio.sources.fixture]
provider = "external"
mode = "playback"
target = "explicit-test-source"
executable = "$helper"
args = ["--external-test", "--modulate"]
[output."HEADLESS-1"]
position = [0, 0]
[output."HEADLESS-2"]
position = [1280, 0]
screen_effect = "off"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
python3 - <<'PY'
import json
import math
import os
import subprocess
import time

umbriel = os.environ["UMBRIEL"]
image = os.path.join(os.environ["UMBRIEL_RUNTIME_DIR"], "audio-cadence.png")

def run(*args):
    return subprocess.check_output([umbriel, *args], text=True, timeout=10)

def source():
    return next(s for s in json.loads(run("effects", "--json"))["audio"] if s["name"] == "fixture")

def frames():
    return {o["name"]: o["effect_frames"] for o in json.loads(run("effect-frames", "--json"))["outputs"]}

def pixel():
    subprocess.run(["grim", "-s", "1", "-o", "HEADLESS-1", image], check=True, timeout=10)
    return subprocess.check_output([os.environ["UMBRIEL_PIXEL_PROBE"], image, "pixel", "900", "600"], text=True).strip()

deadline = time.monotonic() + 4
while not source()["available"]:
    assert time.monotonic() < deadline, "changing provider never became available"
    time.sleep(.02)

# Measure wall time because provider acquisition intentionally ignores the test
# animation clock. IPC overhead counts toward the permitted cap interval.
start = time.monotonic()
before = frames()
time.sleep(1.4)
after = frames()
elapsed = time.monotonic() - start
delta = after["HEADLESS-1"] - before["HEADLESS-1"]
assert 0 < delta <= math.ceil(elapsed * 8) + 2, ("audio cadence violated cap", delta, elapsed)
assert after["HEADLESS-2"] == before["HEADLESS-2"], "audio woke unrelated output"

run("clock-freeze")
run("settle")
first = pixel()
frozen = frames()
sequence = source()["sequence"]
time.sleep(.35)
assert source()["sequence"] > sequence, "provider stopped; freeze control was vacuous"
assert pixel() == first, "capture advanced frozen audio snapshot"
assert frames() == frozen, "frozen audio continued requesting effect-only frames"

run("clock-resume")
deadline = time.monotonic() + 2
while frames()["HEADLESS-1"] == frozen["HEADLESS-1"]:
    assert time.monotonic() < deadline, "audio cadence did not resume"
    time.sleep(.02)
print("changing audio obeyed 8 fps cap, frozen snapshots survived captures, resume worked, second output stayed idle")
PY
