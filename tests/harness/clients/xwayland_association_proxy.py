#!/usr/bin/env python3
"""Hold selected X11 association events while real Xwayland commits Wayland buffers.

Xwayland's XWM and Wayland sockets are independent. Delaying only an association
ClientMessage makes its first buffer commit arrive before the X11 association.
The check releases that event after observing the real buffer import, so this
does not depend on a timer winning a scheduler race.

An optional position hold leaves a real ConfigureNotify queued while association
and Wayland buffers proceed. Presentation barriers for that window stay behind
its geometry until the check releases the hold.
"""

import os
import select
import socket
import struct
import sys
import time


def main():
    arguments = sys.argv[1:]
    wm_index = arguments.index("-wm") + 1
    compositor = socket.socket(fileno=int(arguments[wm_index]))
    server, child_socket = socket.socketpair()
    child_socket.set_inheritable(True)
    arguments[wm_index] = str(child_socket.fileno())
    child_pid = os.fork()
    if child_pid == 0:
        compositor.close()
        server.close()
        binary = os.environ["UMBRIEL_XWAYLAND_REAL_BINARY"]
        os.execv(binary, [binary, *arguments])

    child_socket.close()
    # Only Xwayland keeps the inherited Wayland, display-ready, and listening
    # sockets. Retaining them in the proxy would interfere with lazy teardown.
    keep = {0, 1, 2, compositor.fileno(), server.fileno()}
    for name in os.listdir("/proc/self/fd"):
        descriptor = int(name)
        if descriptor not in keep:
            try:
                os.close(descriptor)
            except OSError:
                pass

    control = os.open(
        os.environ["UMBRIEL_XWAYLAND_ASSOCIATION_CONTROL"],
        os.O_RDWR | os.O_NONBLOCK,
    )
    with open(os.environ["UMBRIEL_XWAYLAND_ASSOCIATION_LOG"], "a", buffering=1) as log:
        trace = lambda message: print(f"{time.monotonic():.6f} {message}", file=log)
        endian = None
        client_setup = True
        server_setup = True
        requests = bytearray()
        responses = bytearray()
        commands = bytearray()
        request_sequence = 0
        latest_sequence = 0
        atom_requests = {}
        association_atoms = set()
        armed = set()
        held = {}
        positions = {}
        trace(f"ready proxy={os.getpid()} xwayland={child_pid}")

        try:
            while True:
                readable, _, _ = select.select([compositor, server, control], [], [])
                if control in readable:
                    commands += os.read(control, 4096)
                    while b"\n" in commands:
                        line, _, commands = commands.partition(b"\n")
                        command, window_text = line.decode().split()
                        window = int(window_text, 0)
                        if command == "hold":
                            armed.add(window)
                            trace(f"armed window={window}")
                        elif command == "release":
                            packets = held.pop(window)
                            armed.remove(window)
                            for packet in packets:
                                # ClientMessage's sequence reports the XWM's last
                                # processed request. Its payload is unchanged, but
                                # delayed events must not rewind libxcb's sequence
                                # tracking past replies forwarded in the meantime.
                                struct.pack_into(endian + "H", packet, 2, latest_sequence)
                                compositor.sendall(packet)
                            trace(f"released window={window} events={len(packets)}")
                        elif command == "hold-position":
                            positions[window] = []
                            trace(f"armed-position window={window}")
                        elif command in ("release-position", "release-oldest-position"):
                            packets = positions[window]
                            if command == "release-oldest-position":
                                end = next(i for i, packet in enumerate(packets) if packet[0] & 0x7F == 33) + 1
                                packets, positions[window] = packets[:end], packets[end:]
                            else:
                                positions.pop(window)
                            # Replies may have passed these held events. Keep
                            # their geometry authoritative before any fence.
                            for packet in sorted(packets, key=lambda value: value[0] & 0x7F != 22):
                                struct.pack_into(endian + "H", packet, 2, latest_sequence)
                                compositor.sendall(packet)
                            trace(f"released-{command.removeprefix('release-')} window={window} events={len(packets)}")
                        else:
                            raise ValueError(f"unknown control command: {command}")

                if compositor in readable:
                    data = compositor.recv(65536)
                    if not data:
                        break
                    if endian is None:
                        endian = "<" if data[0] == ord("l") else ">"
                    server.sendall(data)
                    requests += data
                    if client_setup:
                        if len(requests) < 12:
                            continue
                        name_length, data_length = struct.unpack_from(endian + "HH", requests, 6)
                        length = 12 + ((name_length + 3) & ~3) + ((data_length + 3) & ~3)
                        if len(requests) < length:
                            continue
                        del requests[:length]
                        client_setup = False
                    while len(requests) >= 4:
                        length = struct.unpack_from(endian + "H", requests, 2)[0] * 4
                        if length == 0:
                            if len(requests) < 8:
                                break
                            length = struct.unpack_from(endian + "I", requests, 4)[0] * 4
                        if len(requests) < length:
                            break
                        request_sequence = (request_sequence + 1) & 0xFFFF
                        if requests[0] == 16:  # InternAtom
                            name_length = struct.unpack_from(endian + "H", requests, 4)[0]
                            name = bytes(requests[8 : 8 + name_length])
                            if name in (b"WL_SURFACE_SERIAL", b"WL_SURFACE_ID"):
                                atom_requests[request_sequence] = name.decode()
                        del requests[:length]

                if server in readable:
                    data = server.recv(65536)
                    if not data:
                        break
                    responses += data
                    if server_setup:
                        if len(responses) < 8:
                            continue
                        length = 8 + struct.unpack_from(endian + "H", responses, 6)[0] * 4
                        if len(responses) < length:
                            continue
                        compositor.sendall(responses[:length])
                        del responses[:length]
                        server_setup = False
                    while len(responses) >= 32:
                        kind = responses[0] & 0x7F
                        length = 32
                        if kind in (1, 35):  # Reply or GenericEvent
                            length += struct.unpack_from(endian + "I", responses, 4)[0] * 4
                        if len(responses) < length:
                            break
                        packet = bytearray(responses[:length])
                        del responses[:length]
                        latest_sequence = struct.unpack_from(endian + "H", packet, 2)[0]
                        if kind == 1 and latest_sequence in atom_requests:
                            atom = struct.unpack_from(endian + "I", packet, 8)[0]
                            association_atoms.add(atom)
                            trace(f"atom name={atom_requests.pop(latest_sequence)} value={atom}")
                        if kind == 33:  # ClientMessage
                            window, atom, serial_low, serial_high = struct.unpack_from(endian + "IIII", packet, 4)
                            if window in armed and atom in association_atoms:
                                held.setdefault(window, []).append(packet)
                                trace(f"held window={window} serial={serial_high << 32 | serial_low}")
                                continue
                            # Shape/RandR wakeups carry no subject window. A
                            # presentation fence does, so preserve its ordering
                            # after any position held for that subject.
                            if packet[1] == 32 and atom == 0 and serial_low in positions:
                                positions[serial_low].append(packet)
                                trace(f"held-position-fence window={serial_low}")
                                continue
                        if kind == 22:  # ConfigureNotify
                            window = struct.unpack_from(endian + "I", packet, 8)[0]
                            x, y = struct.unpack_from(endian + "hh", packet, 16)
                            if window in positions and (x, y) == (930, 550):
                                positions[window].append(packet)
                                trace(f"held-position window={window} x={x} y={y}")
                                continue
                        compositor.sendall(packet)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            compositor.close()
            server.close()
            os.close(control)
            trace("closed")
            os.waitpid(child_pid, 0)


if __name__ == "__main__":
    main()
