#!/usr/bin/env python3
"""Device-free PipeWire/WirePlumber integration test; never uses the desktop server."""

import array
import json
import math
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

with tempfile.TemporaryDirectory(prefix="uma.") as directory:
    root = Path(directory)
    config = root / "pipewire.conf"
    config.write_text("""context.properties = { core.daemon = true core.name = test }
context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }
context.modules = [
 { name = libpipewire-module-protocol-native }
 { name = libpipewire-module-client-node }
 { name = libpipewire-module-metadata }
 { name = libpipewire-module-access args = { access.force = unrestricted } }
 { name = libpipewire-module-spa-node-factory }
 { name = libpipewire-module-adapter }
 { name = libpipewire-module-link-factory }
 { name = libpipewire-module-session-manager }
]
context.objects = [
 { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Dummy-Driver priority.driver = 20000 } }
 { factory = adapter args = { factory.name = support.null-audio-sink node.name = test-sink media.class = Audio/Sink audio.position = [ FL FR ] } }
]
""")
    env = dict(
        os.environ,
        XDG_RUNTIME_DIR=directory,
        PIPEWIRE_RUNTIME_DIR=directory,
        PIPEWIRE_REMOTE=str(root / "test"),
        XDG_CONFIG_HOME=str(root / "config"),
        XDG_STATE_HOME=str(root / "state"),
        GIO_USE_VFS="local",
        GSETTINGS_BACKEND="memory",
    )
    env.pop("DBUS_SESSION_BUS_ADDRESS", None)
    children = []
    log = open(root / "log", "w+")
    parent = None
    policy = None
    try:
        daemon = subprocess.Popen(
            ["pipewire", "-c", str(config)], env=env, stdout=log, stderr=log
        )
        children.append(daemon)
        end = time.monotonic() + 3
        while not (root / "test").exists() and time.monotonic() < end:
            time.sleep(0.02)
        policy = subprocess.Popen(
            ["dbus-run-session", "--", "wireplumber", "-p", "policy"],
            env=env,
            stdout=log,
            stderr=log,
            start_new_session=True,
        )
        children.append(policy)
        data = array.array("h")
        for i in range(48000 * 4):
            v = (
                int(32767 * 0.1 * math.sin(2 * math.pi * 440 * i / 48000))
                if i < 48000 * 2
                else 0
            )
            data.extend((v, -v))
        with wave.open(str(root / "signal.wav"), "wb") as f:
            f.setnchannels(2)
            f.setsampwidth(2)
            f.setframerate(48000)
            f.writeframes(data.tobytes())
        parent, child = socket.socketpair()
        parent.settimeout(3)
        helper = subprocess.Popen(
            [sys.argv[1]], stdin=child, env=env, stdout=log, stderr=log
        )
        children.append(helper)
        child.close()
        levels = []
        with parent.makefile("rb") as replies:
            # Wait for policy to create the capture links before playing the signal.
            started = time.monotonic()
            player = None
            while time.monotonic() - started < 8:
                msg = json.loads(replies.readline())
                levels.append((time.monotonic() - started, msg["level"]))
                parent.sendall(b'{"ok":true}\n')
                if player is None and time.monotonic() - started > 2:
                    player = subprocess.Popen(
                        ["pw-cat", "--playback", str(root / "signal.wav")],
                        env=env,
                        stdout=log,
                        stderr=log,
                    )
                    children.append(player)
            print(
                "range",
                min(x[1] for x in levels),
                max(x[1] for x in levels),
                "tail",
                levels[-1],
                flush=True,
            )
            graph = json.loads(subprocess.check_output(["pw-dump"], env=env))
            nodes = {
                n["id"]: n.get("info", {}).get("props", {})
                for n in graph
                if n["type"] == "PipeWire:Interface:Node"
            }
            ours = [
                key
                for key, value in nodes.items()
                if value.get("node.name") == "umbriel-audio"
            ]
            links = [
                n["info"]
                for n in graph
                if n["type"] == "PipeWire:Interface:Link"
                and n["info"]["input-node-id"] in ours
            ]
            assert len(links) == 2 and all(
                nodes[n["output-node-id"]].get("node.name") == "test-sink"
                for n in links
            )
            peak = max(x[1] for x in levels)
            assert 0.60 < peak < 0.70
            # A tick can fall between PipeWire's buffers; the level must hold steady through a steady tone.
            hot = [x for x in levels if x[1] > 0.5 * peak]
            steady = [v for t, v in hot if hot[0][0] + 0.4 < t < hot[-1][0] - 0.4]
            assert steady and max(steady) - min(steady) < 0.05
            assert levels[-1][1] == 0
        parent.close()
        parent = None
        assert helper.wait(timeout=2) == 0
        # Without a socket on stdin the helper dials $UMBRIEL_SOCKET.
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(root / "umbriel.sock"))
        server.listen(1)
        server.settimeout(3)
        bare = {k: v for k, v in env.items() if k != "UMBRIEL_SOCKET"}
        dialled = subprocess.Popen(
            [sys.argv[1]],
            stdin=subprocess.DEVNULL,
            env=bare | {"UMBRIEL_SOCKET": str(root / "umbriel.sock")},
            stdout=log,
            stderr=log,
        )
        children.append(dialled)
        peer, _ = server.accept()
        peer.settimeout(3)
        assert json.loads(peer.recv(256))["cmd"] == "effect-audio"
        peer.close()
        assert dialled.wait(timeout=2) == 0
        server.close()
        # With nothing to dial the helper reports it and exits instead of waiting.
        assert subprocess.run(
            [sys.argv[1]], stdin=subprocess.DEVNULL, env=bare, stderr=log, timeout=3
        ).returncode == 1
        # A nonresponsive IPC peer must not leave capture running.
        parent, child = socket.socketpair()
        parent.settimeout(3)
        blocked = subprocess.Popen(
            [sys.argv[1]], stdin=child, env=env, stdout=log, stderr=log
        )
        children.append(blocked)
        child.close()
        assert parent.recv(256)
        assert blocked.wait(timeout=2) == 0
        parent.close()
        parent = None
        # Backend loss must terminate the helper so the compositor can restart it.
        parent, child = socket.socketpair()
        parent.settimeout(3)
        lost = subprocess.Popen(
            [sys.argv[1]], stdin=child, env=env, stdout=log, stderr=log
        )
        children.append(lost)
        child.close()
        assert parent.recv(256)
        parent.sendall(b'{"ok":true}\n')
        daemon.terminate()
        daemon.wait(timeout=3)
        assert lost.wait(timeout=2) == 1
        parent.close()
        parent = None
        print(
            "PASS: playback routing, opposite-phase RMS, silence, EOF, socket dialling, stalled peer, backend loss"
        )
    except:
        log.flush()
        log.seek(0)
        print(log.read())
        raise
    finally:
        if parent:
            parent.close()
        for p in reversed(children):
            if p.poll() is None:
                if p is policy:
                    os.killpg(p.pid, signal.SIGTERM)
                else:
                    p.terminate()
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
