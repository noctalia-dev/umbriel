#!/usr/bin/env bash
# Target-changing screencast actions require one confirmation per active share.
# Dismissing the warning cancels only the pending action, ending the share
# revokes approval, and the explicit configuration bypass skips the prompt.
set -euo pipefail

readonly POINTER="$UMBRIEL_POINTER_CLIENT"
readonly BTN_LEFT=272

python3 - "$UMBRIEL_SOCKET" "$UMBRIEL" "$POINTER" "$BTN_LEFT" <<'PY'
import json
import select
import socket
import subprocess
import sys

socket_path, umbriel, pointer, button = sys.argv[1:]


def read_line(client, buffer):
    while b"\n" not in buffer:
        try:
            chunk = client.recv(4096)
        except TimeoutError:
            raise SystemExit("timed out waiting for a screencast IPC response") from None
        if not chunk:
            raise SystemExit("screencast subscription closed unexpectedly")
        buffer += chunk
    line, buffer = buffer.split(b"\n", 1)
    return json.loads(line), buffer


def action(name):
    result = subprocess.run([umbriel, "msg", name], capture_output=True, text=True, timeout=5)
    if result.returncode != 0:
        raise SystemExit(f"{name} was rejected: {result.stderr.strip()}")


def expect_no_event(client, reason):
    if select.select([client], [], [], 0.35)[0]:
        event, _ = read_line(client, b"")
        raise SystemExit(f"{reason} unexpectedly published {event!r}")


client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.settimeout(2)
client.connect(socket_path)
client.sendall(b'{"cmd":"subscribe","events":["screencast"]}\n')
event, buffer = read_line(client, b"")
if event.get("event") != "screencast" or event.get("data", {}).get("serial") != 0:
    raise SystemExit(f"unexpected initial screencast event: {event!r}")

client.sendall(b'{"cmd":"screencast-session","active":true}\n')
response, buffer = read_line(client, buffer)
if response != {"ok": None}:
    raise SystemExit(f"activating the screencast returned {response!r}")

output = json.loads(
    subprocess.run([umbriel, "outputs", "--json"], check=True, capture_output=True, text=True, timeout=5).stdout
)[0]["name"]
set_output = f"screencast-set-output:{output}"

# The first request opens the warning and publishes nothing. A click dismisses
# it, so the next request must open a fresh warning instead of being treated as
# approval.
action(set_output)
expect_no_event(client, "the first target change")
subprocess.run([pointer, "1280", "720", "click", button], check=True, timeout=5)
action(set_output)
expect_no_event(client, "the target change after dismissal")

# Repeating a protected action while its warning is visible confirms it. Once
# confirmed, further target-changing actions publish immediately.
action(set_output)
event, buffer = read_line(client, buffer)
data = event.get("data", {})
if data.get("kind") != "output" or data.get("output") != output or data.get("serial") != 1:
    raise SystemExit(f"confirmation published the wrong target: {event!r}")
action("screencast-follow-window")
event, buffer = read_line(client, buffer)
if event.get("data", {}).get("kind") != "follow_window" or event.get("data", {}).get("serial") != 2:
    raise SystemExit(f"an approved follow action did not publish immediately: {event!r}")

# Ending the share revokes approval. A new share must confirm again. The portal
# can publish both transitions before the compositor next wakes for its socket,
# so both complete requests must be drained from one read.
transitions = b"".join(
    json.dumps({"cmd": "screencast-session", "active": active}).encode() + b"\n"
    for active in (False, True)
)
client.sendall(transitions)
for active in (False, True):
    response, buffer = read_line(client, buffer)
    if response != {"ok": None}:
        raise SystemExit(f"setting screencast active={active} returned {response!r}")
action("screencast-follow-output")
expect_no_event(client, "the first action in a new share")
action("screencast-follow-output")
event, buffer = read_line(client, buffer)
if event.get("data", {}).get("kind") != "follow_output" or event.get("data", {}).get("serial") != 3:
    raise SystemExit(f"the new share confirmation published the wrong event: {event!r}")
client.sendall(b'{"cmd":"screencast-session","active":false}\n')
response, buffer = read_line(client, buffer)
if response != {"ok": None}:
    raise SystemExit(f"ending the confirmation test share returned {response!r}")
client.close()
PY

printf '\n[screencast]\ndisable_dynamic_confirmation = true\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

python3 - "$UMBRIEL_SOCKET" "$UMBRIEL" <<'PY'
import json
import socket
import subprocess
import sys

socket_path, umbriel = sys.argv[1:]
client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect(socket_path)
client.sendall(b'{"cmd":"subscribe","events":["screencast"]}\n')
buffer = b""


def read_line():
    global buffer
    while b"\n" not in buffer:
        buffer += client.recv(4096)
    line, buffer = buffer.split(b"\n", 1)
    return json.loads(line)


initial = read_line()
if initial.get("data", {}).get("serial") != 3:
    raise SystemExit(f"unexpected command before bypass check: {initial!r}")
client.sendall(b'{"cmd":"screencast-session","active":true}\n')
if read_line() != {"ok": None}:
    raise SystemExit("could not activate the bypass test share")
result = subprocess.run([umbriel, "msg", "screencast-follow-window"], capture_output=True, text=True, timeout=5)
if result.returncode != 0:
    raise SystemExit(f"configured bypass rejected the action: {result.stderr.strip()}")
event = read_line()
if event.get("data", {}).get("kind") != "follow_window" or event.get("data", {}).get("serial") != 4:
    raise SystemExit(f"configured bypass did not publish immediately: {event!r}")
client.sendall(b'{"cmd":"screencast-session","active":false}\n')
if read_line() != {"ok": None}:
    raise SystemExit("could not end the bypass test share")
client.close()
PY

echo "screencast target changes confirm once per share, retry after dismissal, and honor the bypass"
