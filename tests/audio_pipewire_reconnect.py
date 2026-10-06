#!/usr/bin/env python3
"""Private server lifecycle fixture; never loads an audio node or device."""

import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time


def main():
    helper_executable, daemon_executable, dump_executable = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="umbriel-audio-reconnect-") as directory:
        config = directory + "/pipewire.conf"
        with open(config, "w", encoding="utf-8") as stream:
            stream.write("""context.properties = { core.daemon = true core.name = umbriel-test }
context.modules = [
 { name = libpipewire-module-protocol-native }
 { name = libpipewire-module-client-node }
 { name = libpipewire-module-metadata }
 { name = libpipewire-module-access args = { access.force = unrestricted } }
]
""")
        remote = directory + "/umbriel-test"
        environment = dict(os.environ, XDG_RUNTIME_DIR=directory,
                           PIPEWIRE_RUNTIME_DIR=directory, PIPEWIRE_REMOTE=remote)
        host, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        helper = subprocess.Popen([helper_executable], stdin=child, env=environment,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        child.close()
        host.settimeout(3)
        daemon = None

        def receive_type():
            packet = host.recv(256)
            assert len(packet) >= 16 and packet[:4] == b"UAF1", "invalid protocol packet"
            return struct.unpack_from("<H", packet, 6)[0]

        def wait_client():
            deadline = time.monotonic() + 6
            while time.monotonic() < deadline:
                result = subprocess.run([dump_executable, "-r", remote], env=environment,
                                        capture_output=True, timeout=2, check=False)
                if result.returncode == 0:
                    entries = json.loads(result.stdout)
                    assert not any(entry.get("type") == "PipeWire:Interface:Node"
                                   for entry in entries), "private server must have no audio nodes"
                    if any(str(entry.get("info", {}).get("props", {}).get("application.process.id"))
                           == str(helper.pid) for entry in entries):
                        return
                time.sleep(0.1)
            raise AssertionError("helper did not reconnect")

        try:
            message = bytearray(32)
            message[0:4] = b"UAF1"
            struct.pack_into("<HHHH", message, 4, 1, 1, 32, 1)
            struct.pack_into("<QHH", message, 16, 29, 1, 2)
            host.send(message)
            assert receive_type() == 2, "missing READY"
            assert receive_type() == 5, "initial absence must be UNAVAILABLE"
            for cycle in range(2):
                with open(directory + "/daemon.log", "ab") as log:
                    daemon = subprocess.Popen([daemon_executable, "-c", config], env=environment,
                                              stdout=log, stderr=log)
                wait_client()
                print("connected to private empty server", cycle + 1)
                daemon.terminate()
                daemon.wait(timeout=3)
                daemon = None
                # Drain unavailable heartbeats while the backend is absent.
                deadline = time.monotonic() + 1.5
                while time.monotonic() < deadline:
                    assert receive_type() == 5, "a device-free server must never produce snapshots"
            host.close()
            assert helper.wait(timeout=2) == 0, "helper failed to exit on EOF"
            print("PASS: absence, reconnect, loss, reconnect, no snapshots, EOF exit")
        finally:
            host.close()
            if daemon is not None and daemon.poll() is None:
                daemon.terminate()
                daemon.wait(timeout=3)
            if helper.poll() is None:
                helper.kill()
                helper.wait()


if __name__ == "__main__":
    main()
