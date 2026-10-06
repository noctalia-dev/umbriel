#!/usr/bin/env bash
# harness: outputs=2
# TIME damage is acknowledged per successful output submission, including an
# explicit frozen-clock step while another output cannot submit its buffer.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
cat > "$UMBRIEL_RUNTIME_DIR/time-straddling.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(fract(umbriel_time * 0.3), 0.7, 0.0, 1.0); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false
[appearance.shadow]
enabled = false
[effects]
window = "time-straddling"
in_capture = true
max_fps = 8
[effects.preset.time-straddling]
kind = "window"
shader = "time-straddling.glsl"
[output.HEADLESS-1]
position = [0, 0]
[output.HEADLESS-2]
position = [1280, 0]
[[window_rule]]
match.title = "^time-straddling$"
default_output = "HEADLESS-1"
default_floating = true
default_position = { x = 700, y = 100, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFF000000 "$UMBRIEL_UNMAP_CLIENT" time-straddling 400 300 > "$UMBRIEL_RUNTIME_DIR/time-client.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "time-straddling")')
  [[ -n $window ]] && break
  sleep .025
done
[[ -n $window ]]
read -r x y w h < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
"$UMBRIEL" settle > /dev/null
pointer_hold 2560 720 move "$((x+w/2))" "$((y+h/2))" mod super press 272 move 1250 "$((y+h/2))" -- release 272 mod none
python3 - <<'PY'
import json
import os
import subprocess
import time

umbriel = os.environ["UMBRIEL"]
def run(*args):
    return subprocess.check_output([umbriel, *args], text=True, timeout=10)
def frames():
    return {o["name"]: o for o in json.loads(run("effect-frames", "--json"))["outputs"]}
def pixel(name):
    image = os.path.join(os.environ["UMBRIEL_RUNTIME_DIR"], name + ".png")
    subprocess.run(["grim", "-s", "1", "-o", name, image], check=True, timeout=10)
    x = "1200" if name == "HEADLESS-1" else "50"
    value = subprocess.check_output([os.environ["UMBRIEL_PIXEL_PROBE"], image, "pixel", x, "250"], text=True)
    rgb = tuple(map(int, value.split()))
    assert rgb[1] > 120, ("fixture did not straddle", name, rgb)
    return rgb
run("clock-freeze")
run("settle")
first = {name: pixel(name) for name in frames()}
assert first["HEADLESS-1"] == first["HEADLESS-2"], first
before = frames()
run("output-commit-hold", "HEADLESS-1 on")
# clock-advance waits for output drawing; submission failure must not acknowledge TIME.
run("clock-advance", "500")
deadline = time.monotonic() + 4
while True:
    state = frames()
    if state["HEADLESS-1"]["rejected_buffer_commits"] > before["HEADLESS-1"]["rejected_buffer_commits"]:
        break
    assert time.monotonic() < deadline, ("no held submission", state)
    time.sleep(.01)
assert state["HEADLESS-1"]["buffer_commits"] == before["HEADLESS-1"]["buffer_commits"], state
second = pixel("HEADLESS-2")
assert second != first["HEADLESS-2"], ("healthy output did not advance TIME", first, second)
# Real time exercises backend retries while the animation clock remains frozen.
quiet = frames()
time.sleep(.25)
after = frames()
assert after["HEADLESS-1"]["rejected_buffer_commits"] > quiet["HEADLESS-1"]["rejected_buffer_commits"], after
assert after["HEADLESS-2"]["buffer_commits"] == quiet["HEADLESS-2"]["buffer_commits"], ("failed TIME rebind woke healthy output", quiet, after)
run("output-commit-hold", "HEADLESS-1 off")
run("settle")
assert pixel("HEADLESS-1") == second, ("failed output acknowledged TIME before submitting", second)
assert pixel("HEADLESS-2") == second, "recovering output changed healthy TIME"
print("per-output TIME steps survived failed submission without waking the healthy straddling occurrence")
PY
