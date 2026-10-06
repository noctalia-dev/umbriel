#!/usr/bin/env bash
# A built buffer rejected at submission cannot consume its audio latch. New
# frozen injection stays queued until the old snapshot successfully retries.
set -euo pipefail
helper=$(realpath "$(dirname "$UMBRIEL_UNMAP_CLIENT")/audio-synthetic")
cat > "$UMBRIEL_RUNTIME_DIR/audio-failed.glsl" <<'GLSL'
vec4 screen(vec2 uv) { return vec4(umbriel_audio_rms(), 0.0, 0.0, 1.0); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
enabled = false
[effects]
screen = "audio-failed"
in_capture = true
max_fps = 2
[effects.preset.audio-failed]
kind = "screen"
shader = "audio-failed.glsl"
audio = "fixture"
[effects.audio.sources.fixture]
provider = "external"
mode = "playback"
target = "explicit-test-source"
executable = "$helper"
args = ["--external-test", "--silence"]
EOF
"$UMBRIEL" msg config-reload > /dev/null
for _ in $(seq 100); do
  (( $(grep -c 'config reloaded' "$UMBRIEL_RUNTIME_DIR/compositor.log") >= 2 )) && break
  sleep 0.02
done
(( $(grep -c 'config reloaded' "$UMBRIEL_RUNTIME_DIR/compositor.log") >= 2 ))
python3 - <<'PY'
import json
import os
import subprocess
import time

binary = os.environ["UMBRIEL"]
def run(*args):
    return subprocess.check_output([binary, *args], text=True, timeout=10)
def output():
    return json.loads(run("effect-frames", "--json"))["outputs"][0]
def latch(state):
    return next(a for a in state["audio"] if a["source"] == "fixture")
def inject(value):
    run("audio-inject", json.dumps(dict(source="fixture", rms=value, peak=value, envelope=value, bands=[value]*16)))
def wait(predicate, reason):
    deadline = time.monotonic() + 4
    while True:
        state = output()
        if predicate(state):
            return state
        assert time.monotonic() < deadline, (reason, state)
        time.sleep(.01)

deadline = time.monotonic() + 4
while not json.loads(run("effects", "--json"))["audio"][0]["available"]:
    assert time.monotonic() < deadline, "provider unavailable"
    time.sleep(.01)
run("clock-freeze")
inject(0)
run("settle")
before = output()
consumed = latch(before)["consumed_revision"]
run("output-commit-hold", "HEADLESS-1 on")
inject(.25)
first = wait(lambda s: s["rejected_buffer_commits"] > before["rejected_buffer_commits"], "no rejected buffer")
assert first["buffer_commits"] == before["buffer_commits"], ("held frame committed", first)
assert latch(first)["pending"] and latch(first)["latched_rms"] == .25, first
assert latch(first)["presented_rms"] == 0 and latch(first)["consumed_revision"] == consumed, first
inject(.75)
second = wait(lambda s: s["rejected_buffer_commits"] > first["rejected_buffer_commits"], "failed snapshot never retried")
assert latch(second)["latched_rms"] == .25, ("pending snapshot was replaced", second)
assert latch(second)["consumed_revision"] == consumed and second["buffer_commits"] == before["buffer_commits"], second
run("output-commit-hold", "HEADLESS-1 off")
final = wait(lambda s: not latch(s)["pending"] and latch(s)["presented_rms"] == .75, "queued injection did not submit")
assert final["buffer_commits"] >= before["buffer_commits"] + 2, ("old and queued values did not each submit", before, final)
assert latch(final)["consumed_revision"] == latch(final)["revision"], final
image = os.path.join(os.environ["UMBRIEL_RUNTIME_DIR"], "audio-retry.png")
subprocess.run(["grim", "-s", "1", "-o", "HEADLESS-1", image], check=True, timeout=10)
pixel = subprocess.check_output([os.environ["UMBRIEL_PIXEL_PROBE"], image, "pixel", "900", "600"], text=True)
r, g, b = map(int, pixel.split())
assert 170 < r < 210 and g < 20 and b < 20, ("submitted audio did not reach pixels", pixel)
print("failed buffer submissions preserved pending audio, consumed only after success, and replayed queued frozen injection")
PY
