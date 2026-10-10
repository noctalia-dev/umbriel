#!/usr/bin/env bash
# Own instance: its PATH contains a deterministic helper, never a desktop audio recorder.
set -euo pipefail
python3 - <<'PY'
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

root = Path(os.environ['UMBRIEL_RUNTIME_DIR']) / 'automatic'
root.mkdir()
helper = root / 'umbriel-audio'
helper.write_text('''#!/usr/bin/env python3
import json, os, socket, time
from pathlib import Path
log = Path(os.environ['XDG_RUNTIME_DIR']) / 'helper.log'
def record(event):
    with log.open('a') as f:
        f.write(f'{event} {os.getpid()}\\n')
record('start')
sock = socket.socket(fileno=0)
sock.setblocking(True)
sock.settimeout(1)
try:
    with sock.makefile('rb') as replies:
        while True:
            sock.sendall(b'{"cmd":"effect-audio","version":1,"level":0.5}\\n')
            if json.loads(replies.readline()) != {'ok': True}:
                break
            time.sleep(0.04)
except (OSError, ValueError):
    pass
finally:
    record('stop')
''')
helper.chmod(0o755)
(root / 'audio.glsl').write_text('vec4 screen(vec2 uv) { return vec4(umbriel_audio_level(), umbriel_audio_available(), 0.0, 1.0); }')
config = root / 'config.toml'
config.write_text('''[general]
xwayland = false
show_cheatsheet = false
autostart = []
[animation]
enabled = false
[keybinds]
"Mod+A" = "effect-screen-set:audio"
[output.HEADLESS-1]
mode = "320x240@60"
[output.HEADLESS-2]
mode = "320x240@60"
[effects]
in_capture = true
[effects.preset.audio]
kind = "screen"
shader = "audio.glsl"
''')
env = dict(os.environ, XDG_RUNTIME_DIR=str(root), WAYLAND_DISPLAY='wayland-0',
           UMBRIEL_SOCKET=str(root / 'umbriel-wayland-0.sock'),
           PATH=str(root) + ':' + os.environ['PATH'], WLR_BACKENDS='headless',
           WLR_HEADLESS_OUTPUTS='2', WLR_LIBINPUT_NO_DEVICES='1')
for key in ('DISPLAY', 'DBUS_SESSION_BUS_ADDRESS'):
    env.pop(key, None)
exe = os.environ['UMBRIEL']

def run(*args):
    return subprocess.check_output(args, env=env, text=True, timeout=5)

def wait_for(predicate, description):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError(description)

def events(event):
    log = root / 'helper.log'
    return [int(line.split()[1]) for line in log.read_text().splitlines()
            if line.startswith(event + ' ')] if log.exists() else []

def pixel(expected):
    def matches():
        image = str(root / 'frame.png')
        run('grim', '-o', 'HEADLESS-1', image)
        actual = tuple(map(int, run(os.environ['UMBRIEL_PIXEL_PROBE'], image, 'pixel', '100', '100').split()))
        return all(abs(a - b) < 8 for a, b in zip(actual, expected))
    wait_for(matches, f'audio did not reach pixels {expected}')

with (root / 'compositor.log').open('w') as log:
    compositor = subprocess.Popen([exe, '-c', str(config)], env=env, stdout=log, stderr=log)
    lock = None
    producer = None
    try:
        wait_for(lambda: Path(env['UMBRIEL_SOCKET']).exists(), 'compositor did not boot')
        run(exe, 'settle')
        assert not events('start'), 'unselected preset started recording'
        run(exe, 'msg', 'effect-screen-set:audio/HEADLESS-1')
        wait_for(lambda: len(events('start')) == 1, 'selected effect did not start helper')
        pixel((128, 255, 0))
        run(exe, 'msg', 'effect-screen-set:audio/HEADLESS-2')
        run(exe, 'msg', 'effect-screen-set:off/HEADLESS-1')
        run(exe, 'settle')
        assert len(events('start')) == 1 and not events('stop'), 'second consumer did not share helper'
        run(exe, 'msg', 'effect-screen-set:off/HEADLESS-2')
        wait_for(lambda: len(events('stop')) == 1, 'last consumer did not stop helper')
        run(exe, 'msg', 'effect-screen-set:audio/HEADLESS-1')
        pixel((128, 255, 0))
        assert len(events('start')) == 2

        # A real external producer replaces the automatic helper and owns the feed.
        producer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        producer.settimeout(3)
        producer.connect(env['UMBRIEL_SOCKET'])
        producer.sendall(b'{"cmd":"effect-audio","version":1,"level":1}\n')
        assert json.loads(producer.recv(256)) == {'ok': True}
        wait_for(lambda: len(events('stop')) == 2, 'external producer did not stop helper')
        producer.close()
        producer = None
        wait_for(lambda: len(events('start')) == 3, 'helper did not resume after external producer')
        pixel((128, 255, 0))

        lock = subprocess.Popen([os.environ['UMBRIEL_LOCK_CLIENT']], env=env,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        assert lock.stdout.readline().strip() == 'locked'
        wait_for(lambda: len(events('stop')) == 3, 'lock left recorder running')
        lock.stdin.write('unlock\n')
        lock.stdin.flush()
        assert lock.stdout.readline().strip() == 'unlocked'
        lock.wait(timeout=3)
        lock = None
        pixel((128, 255, 0))
        assert len(events('start')) == 4

        run(exe, 'msg', 'dpms-off')
        wait_for(lambda: len(events('stop')) == 4, 'powered-off outputs left capture running')
        run(exe, 'msg', 'dpms-on')
        pixel((128, 255, 0))
        assert len(events('start')) == 5

        # Recover even though the audio-only effect has no clock to keep drawing.
        os.kill(events('start')[-1], signal.SIGKILL)
        wait_for(lambda: len(events('start')) == 6, 'helper crash did not retry')
        pixel((128, 255, 0))
        compositor.terminate()
        assert compositor.wait(timeout=5) == 0
        wait_for(lambda: len(events('stop')) == 5, 'compositor exit left recorder running')
        print('PASS: selection, pixels, shared demand, takeover, lock, DPMS, restart, shutdown')
    except Exception:
        print((root / 'compositor.log').read_text())
        raise
    finally:
        if producer:
            producer.close()
        if lock and lock.poll() is None:
            lock.kill()
            lock.wait()
        if compositor.poll() is None:
            compositor.terminate()
            compositor.wait(timeout=5)
PY
