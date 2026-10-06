#!/usr/bin/env bash
# harness: outputs=2
# Audio latches for a grabbed window spanning outputs must not turn composition
# rebinding into uncapped cross-output damage, including while snapshots freeze.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
helper=$(realpath "$(dirname "$UMBRIEL_UNMAP_CLIENT")/audio-synthetic")
cat > "$UMBRIEL_RUNTIME_DIR/audio-straddling.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(umbriel_audio_rms(), 0.7, 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/audio-straddling-border.glsl" <<'GLSL'
vec4 border(vec2 uv) { return vec4(0.0, 0.0, umbriel_audio_rms(), 1.0); }
GLSL
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
border = "audio-straddling-border"
window = "audio-straddling"
in_capture = true
max_fps = 8
[effects.preset.audio-straddling]
kind = "window"
shader = "audio-straddling.glsl"
audio = "fixture"
[effects.preset.audio-straddling-border]
kind = "border"
shader = "audio-straddling-border.glsl"
audio = "fixture"
padding = 20
animated = false
[effects.preset.audio-straddling-border.light]
spread = 40
intensity = 4
threshold = 0.2
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
[[window_rule]]
match.title = "^audio-straddling$"
default_output = "HEADLESS-1"
default_floating = true
default_position = { x = 700, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFF000000 "$UMBRIEL_UNMAP_CLIENT" audio-straddling 400 300 > "$UMBRIEL_RUNTIME_DIR/audio-client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "audio-straddling")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
read -r x y w h < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
"$UMBRIEL" settle > /dev/null
pointer_hold 2560 720 move "$((x+w/2))" "$((y+h/2))" mod super press 272 move 1250 "$((y+h/2))" -- release 272 mod none
python3 - <<'PY'
import json
import math
import os
import subprocess
import time

umbriel = os.environ["UMBRIEL"]

def run(*args):
    return subprocess.check_output([umbriel, *args], text=True, timeout=10)

def frames():
    return {o["name"]: o for o in json.loads(run("effect-frames", "--json"))["outputs"]}

deadline = time.monotonic() + 3
while not any(s["available"] for s in json.loads(run("effects", "--json"))["audio"]):
    assert time.monotonic() < deadline, ("straddling provider unavailable", run("effects", "--json"), run("windows", "--json"))
    time.sleep(.02)
# Position IPC reports the layout target until a drag releases; verify the
# actual held presentation spans both outputs by its static green channel.
for name, x in (("HEADLESS-1", 1200), ("HEADLESS-2", 50)):
    image = os.path.join(os.environ["UMBRIEL_RUNTIME_DIR"], name + ".png")
    subprocess.run(["grim", "-s", "1", "-o", name, image], check=True, timeout=10)
    pixel = subprocess.check_output([os.environ["UMBRIEL_PIXEL_PROBE"], image, "pixel", str(x), "250"], text=True)
    assert int(pixel.split()[1]) > 120, ("fixture did not straddle", name, pixel)
before = frames()
start = time.monotonic()
time.sleep(1.4)
after = frames()
limit = math.ceil((time.monotonic() - start) * 8) + 3
for name in before:
    effects = after[name]["effect_frames"] - before[name]["effect_frames"]
    commits = after[name]["buffer_commits"] - before[name]["buffer_commits"]
    assert 0 < effects <= limit, ("straddling effect cap", name, effects, limit)
    assert 0 < commits <= limit, ("straddling damage ping-pong", name, commits, limit)
run("clock-freeze")
run("settle")
before = frames()
time.sleep(.35)
after = frames()
for name in before:
    assert after[name]["buffer_commits"] == before[name]["buffer_commits"], ("frozen straddling output kept committing", name, before, after)

def audio(state, name):
    return next(a for a in state[name]["audio"] if a["source"] == "fixture")

def inject(value):
    run("audio-inject", json.dumps(dict(source="fixture", rms=value, peak=value, envelope=value, bands=[value] * 16)))

def wait(predicate, reason):
    deadline = time.monotonic() + 4
    while True:
        state = frames()
        if predicate(state):
            return state
        assert time.monotonic() < deadline, (reason, state)
        time.sleep(.01)

# Deliberately stagger the two compositions: output 1 must retry .25 while
# output 2 has already presented .75. Rebinding either must not damage the other.
run("output-commit-hold", "HEADLESS-1 on")
inject(.25)
wait(lambda s: audio(s, "HEADLESS-1")["pending"] and audio(s, "HEADLESS-1")["latched_rms"] == .25, "first output never held .25")
inject(.75)
before = wait(lambda s: audio(s, "HEADLESS-2")["presented_rms"] == .75, "second output never presented .75")
assert audio(before, "HEADLESS-1")["latched_rms"] == .25, before
# Real time is necessary here: the failed backend path retries independently
# of the frozen animation clock, and the healthy output must remain quiet.
time.sleep(.25)
after = frames()
assert after["HEADLESS-1"]["rejected_buffer_commits"] > before["HEADLESS-1"]["rejected_buffer_commits"], "failure retry control was vacuous"
assert after["HEADLESS-2"]["buffer_commits"] == before["HEADLESS-2"]["buffer_commits"], ("held-output rebind damaged healthy output", before, after)
run("output-commit-hold", "HEADLESS-1 off")
wait(lambda s: not audio(s, "HEADLESS-1")["pending"] and audio(s, "HEADLESS-1")["presented_rms"] == .75, "staggered first output did not recover")
# The border emission and the window stage share the output's same latch.
# Sampling both output glows catches reuse of another occurrence's light cache.
audio_pixels = {}
for value in (.75, 0.0):
    inject(value)
    wait(lambda s: all(not audio(s, name)["pending"] and audio(s, name)["presented_rms"] == value for name in s), "lit outputs did not consume injection")
    for name, x in (("HEADLESS-1", 1200), ("HEADLESS-2", 50)):
        image = os.path.join(os.environ["UMBRIEL_RUNTIME_DIR"], name + "-light.png")
        subprocess.run(["grim", "-s", "1", "-o", name, image], check=True, timeout=10)
        def pixel(y):
            return [int(c) for c in subprocess.check_output([os.environ["UMBRIEL_PIXEL_PROBE"], image, "pixel", str(x), str(y)], text=True).split()]
        window, ring, glow = pixel(250), pixel(88), pixel(50)
        assert abs(window[0] - round(value * 255)) <= 3, ("window latch mismatch", name, value, window)
        audio_pixels[value, name] = (ring, glow)
        assert (glow[2] > 15 if value else glow[2] <= 2), ("light cache latch mismatch", name, value, glow)
# Compare the audio shader against deterministic literal-input renders. The
# light pass screen-blends over the ring, so its final blue is not raw RMS.
for value in (.75, 0.0):
    with open(os.path.join(os.environ["UMBRIEL_RUNTIME_DIR"], "audio-straddling-border.glsl"), "w") as shader:
        shader.write(f"vec4 border(vec2 uv) {{ return vec4(0.0, 0.0, {value}, 1.0); }}\n")
    run("msg", "config-reload")
    run("settle")
    for name, x in (("HEADLESS-1", 1200), ("HEADLESS-2", 50)):
        image = os.path.join(os.environ["UMBRIEL_RUNTIME_DIR"], name + "-reference.png")
        subprocess.run(["grim", "-s", "1", "-o", name, image], check=True, timeout=10)
        reference = [[int(c) for c in subprocess.check_output([os.environ["UMBRIEL_PIXEL_PROBE"], image, "pixel", str(x), str(y)], text=True).split()] for y in (88, 50)]
        for actual, expected in zip(audio_pixels[value, name], reference):
            assert all(abs(a - e) <= 2 for a, e in zip(actual, expected)), ("lit audio disagreed with literal reference", name, value, actual, expected)
print("straddling audio obeyed cap, froze without commits, and retried one output without damaging the other")
PY
pointer_release
