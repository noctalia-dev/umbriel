#!/usr/bin/env bash
# harness: outputs=2
set -euo pipefail
cat > "$UMBRIEL_RUNTIME_DIR/audio.glsl" <<'GLSL'
vec4 screen(vec2 uv) { return vec4(umbriel_audio_level(), umbriel_audio_available(), 0.0, 1.0); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[animation]
enabled = false
[colors]
backdrop = "#000000FF"
[effects]
screen = "audio"
in_capture = true
max_fps = 10
[effects.preset.audio]
kind = "screen"
shader = "audio.glsl"
[output.HEADLESS-2]
screen_effect = "off"
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
image = str(Path(os.environ['UMBRIEL_RUNTIME_DIR']) / 'audio.png')


def run(*args):
    return subprocess.check_output(args, text=True, timeout=10)


def connect():
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(3)
    sock.connect(os.environ['UMBRIEL_SOCKET'])
    return sock


def send(sock, level):
    sock.sendall((json.dumps({'cmd': 'effect-audio', 'version': 1, 'level': level}) + '\n').encode())
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
    raise AssertionError(f'{output}: {actual}, wanted {expected}; producer: {producer.error}')


class Producer:
    def __init__(self, level):
        self.level = level
        self.changing = False
        self.error = None
        self.stop = threading.Event()
        self.sock = connect()
        assert send(self.sock, level) == {'ok': True}
        self.worker = threading.Thread(target=self.feed, daemon=True)
        self.worker.start()

    def feed(self):
        try:
            while not self.stop.wait(0.02):
                value = (time.monotonic() * 3) % 1 if self.changing else self.level
                assert send(self.sock, value) == {'ok': True}
        except (EOFError, OSError, AssertionError) as error:
            self.error = error

    def finish(self, close=True):
        self.stop.set()
        self.worker.join(3)
        assert not self.worker.is_alive(), 'producer worker stuck'
        if close:
            self.sock.close()


# Fresh silence remains available; unchanged input stays idle.
producer = Producer(0)
pixel((0, 255, 0))
run(exe, 'settle')
before = frames()
time.sleep(0.3)  # real time: identical fresh samples must not request effect frames
assert frames() == before, 'identical samples requested frames'
other = connect()
assert 'err' in send(other, 1), 'a second producer took ownership'
other.close()
producer.level = 1
pixel((255, 255, 0))
pixel((0, 0, 0), 'HEADLESS-2')
assert frames()['HEADLESS-2'] == before['HEADLESS-2'], 'unused output requested audio frames'

# Audio ignores the animation clock and obeys the effect cap.
run(exe, 'clock-freeze')
producer.level = 0
pixel((0, 255, 0))
producer.changing = True
start = time.monotonic()
before = frames()['HEADLESS-1']
time.sleep(0.6)  # real time: measure the existing output effect timer
count = frames()['HEADLESS-1'] - before
assert 1 <= count <= math.ceil((time.monotonic() - start) * 10) + 1, f'uncapped audio frames: {count}'
producer.changing = False
producer.level = 1
pixel((255, 255, 0))
run(exe, 'renderer-recover')
# Recovery can exceed the freshness deadline; reconnect.
producer.finish()
run(exe, 'settle')
producer = Producer(1)
pixel((255, 255, 0))
config = Path(os.environ['UMBRIEL_CONFIG'])
config.write_text(config.read_text().replace('in_capture = true', 'in_capture = false'))
run(exe, 'msg', 'config-reload')
pixel((0, 0, 0))
config.write_text(config.read_text().replace('in_capture = false', 'in_capture = true'))
run(exe, 'msg', 'config-reload')
pixel((255, 255, 0))
assert producer.error is None, f'producer failed: {producer.error}'
producer.finish()
pixel((0, 0, 0))
run(exe, 'settle')
before = frames()
time.sleep(0.3)  # real time: disconnected audio must become idle
assert frames() == before, 'disconnected audio kept drawing'

producer = Producer(1)
pixel((255, 255, 0))
producer.finish(close=False)
assert producer.sock.recv(1) == b'', 'stale producer retained ownership'
producer.sock.close()
pixel((0, 0, 0))
producer = Producer(1)
pixel((255, 255, 0))
producer.finish(close=False)
producer.sock.sendall(b'{"cmd":"effect-audio","version":1,"level":false}\n')
assert b'err' in producer.sock.recv(256)
assert producer.sock.recv(1) == b'', 'invalid producer stayed connected'
producer.sock.close()
pixel((0, 0, 0))

# The 256-byte limit counts the newline; requests arrive one per write; the owner sends only effect-audio.
line = b'{"cmd":"effect-audio","version":1,"level":0.5}'
pad = lambda size: line + b' ' * (size - len(line) - 1) + b'\n'
for data, what in ((pad(257), 'an oversized request'), (line + b'\n' + line + b'\n', 'two requests in one write')):
    probe = connect()
    probe.sendall(data)
    assert b'err' in probe.recv(256), f'{what} got through'
    probe.close()
owner = connect()
owner.sendall(pad(256))
assert b'ok' in owner.recv(256), 'a 256-byte request was refused'
owner.sendall(b'{"cmd":"workspaces"}\n')
assert b'err' in owner.recv(256), 'the producer ran another command'
owner.close()
pixel((0, 0, 0))

producer = Producer(1)
pixel((255, 255, 0))
lock = subprocess.Popen([os.environ['UMBRIEL_LOCK_CLIENT']], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
assert lock.stdout.readline().strip() == 'locked'
producer.worker.join(3)
assert producer.error is not None, 'lock did not disconnect the producer'
producer.finish()
other = connect()
assert 'err' in send(other, 1), 'audio accepted while locked'
other.close()
lock.stdin.write('unlock\n')
lock.stdin.flush()
assert lock.stdout.readline().strip() == 'unlocked'
lock.wait(timeout=5)
pixel((0, 0, 0))
producer = Producer(1)
pixel((255, 255, 0))
producer.finish()
PY
