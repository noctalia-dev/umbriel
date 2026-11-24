#!/usr/bin/env bash
# harness: outputs=2
# Generic live screen uniforms: ownership, snapshot isolation, cleanup and frame pacing.
set -euo pipefail

cat > "$UMBRIEL_RUNTIME_DIR/live.glsl" <<'GLSL'
uniform float external_level;
uniform float external_extra;
uniform float umbriel_live_available;
vec4 screen(vec2 uv) {
  return vec4(external_level, umbriel_live_available, external_extra, 1.0);
}
GLSL
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[animation]
enabled = false
[colors]
backdrop = "#000000FF"
[effects]
screen = "live"
in_capture = true
max_fps = 10
[effects.preset.live]
kind = "screen"
shader = "live.glsl"
TOML
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null

python3 - <<'PY'
import json
import math
import os
from pathlib import Path
import socket
import subprocess
import threading
import time

exe = os.environ['UMBRIEL']
image = str(Path(os.environ['UMBRIEL_RUNTIME_DIR']) / 'live.png')


def run(*args):
    return subprocess.check_output(args, text=True, timeout=10)


def connect():
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(3)
    sock.connect(os.environ['UMBRIEL_SOCKET'])
    return sock


def send(sock, output, values):
    request = {
        'cmd': 'effect-uniforms', 'version': 1, 'kind': 'screen', 'output': output,
        'uniforms': [{'name': name, 'type': 'float', 'value': value} for name, value in values.items()],
    }
    sock.sendall((json.dumps(request) + '\n').encode())
    reply = b''
    while not reply.endswith(b'\n'):
        chunk = sock.recv(256)
        if not chunk:
            raise EOFError('producer disconnected')
        reply += chunk
    return json.loads(reply)


def frames():
    return {o['name']: o['effect_frames'] for o in json.loads(run(exe, 'effect-frames', '--json'))['outputs']}


def pixel(expected, output='HEADLESS-1'):
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline:
        run('grim', '-s', '1', '-o', output, image)
        actual = tuple(map(int, run(os.environ['UMBRIEL_PIXEL_PROBE'], image, 'pixel', '900', '600').split()))
        if all(abs(a-b) < 8 for a, b in zip(actual, expected)):
            return
        time.sleep(0.04)
    raise AssertionError(f'{output}: got {actual}, expected {expected}')


class Producer:
    def __init__(self, output, values):
        self.output = output
        self.values = values
        self.changing = None
        self.error = None
        self.stop = threading.Event()
        self.sock = connect()
        assert send(self.sock, self.output, self.values) == {'ok': True}
        self.worker = threading.Thread(target=self.feed, daemon=True)
        self.worker.start()

    def feed(self):
        try:
            while not self.stop.wait(0.02):
                values = self.values
                changing = self.changing
                if changing is not None:
                    values = {**values, changing: (time.monotonic() * 3) % 1}
                assert send(self.sock, self.output, values) == {'ok': True}
        except (EOFError, OSError, AssertionError) as error:
            self.error = error

    def close(self):
        self.stop.set()
        self.worker.join(3)
        assert not self.worker.is_alive(), 'producer worker stuck'
        self.sock.close()
        assert self.error is None, f'producer failed: {self.error}'


pixel((0, 0, 0))
pixel((0, 0, 0), 'HEADLESS-2')
first = Producer('HEADLESS-1', {'external_level': 0.25, 'external_extra': 0.5})
pixel((64, 255, 128))
second = Producer('HEADLESS-2', {'external_extra': 1.0})
pixel((0, 255, 255), 'HEADLESS-2')

other = connect()
assert 'err' in send(other, 'HEADLESS-1', {'external_level': 1.0}), 'second producer stole the slot'
other.close()

# Both outputs use the same program. An omitted value is zero on each draw.
first.values = {'external_level': 0.75}
pixel((191, 255, 0))
second.values = {'external_extra': 0.25}
pixel((0, 255, 64), 'HEADLESS-2')
pixel((191, 255, 0))

# Fresh identical values and changes to unread uniforms must not drive frames.
run(exe, 'settle')
before = frames()
time.sleep(0.3)  # real time: identical fresh samples must not request effect frames
assert frames() == before, 'identical samples requested frames'
first.changing = 'unused'
time.sleep(0.3)  # real time: unused uniform changes must not request effect frames
assert frames() == before, 'unused uniforms requested frames'

# Changed live input uses the same effect-frame cap as other dynamic inputs.
first.changing = 'external_level'
start = time.monotonic()
before = frames()
time.sleep(0.6)  # real time: measure the output effect timer
after = frames()
count = after['HEADLESS-1'] - before['HEADLESS-1']
assert 1 <= count <= math.ceil((time.monotonic() - start) * 10) + 1, f'uncapped live frames: {count}'
assert after['HEADLESS-2'] == before['HEADLESS-2'], 'unrelated output requested live frames'

first.changing = None
first.values = {'external_level': 1.0}
pixel((255, 255, 0))
first.close()
pixel((0, 0, 0))
pixel((0, 255, 64), 'HEADLESS-2')
first = Producer('HEADLESS-1', {'external_level': 1.0})
pixel((255, 255, 0))
first.close()
second.close()
pixel((0, 0, 0))
pixel((0, 0, 0), 'HEADLESS-2')
PY
