#!/usr/bin/env bash
# The IPC surface answers and returns well-formed JSON of the documented shape.
set -euo pipefail

spawn_client() {
  foot --title="ipc-client" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $want window(s), got $("$UMBRIEL" windows --json | jq 'length'): $("$UMBRIEL" windows --json)"
  return 1
}

python3 - "$UMBRIEL_SOCKET" "$UMBRIEL" <<'PY'
import json
import socket
import subprocess
import sys
import time

socket_path, umbriel = sys.argv[1:]


def connect():
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(socket_path)
    return client


def request(payload):
    client = connect()
    client.sendall(payload)
    response = b""
    while True:
        chunk = client.recv(4096)
        if not chunk:
            break
        response += chunk
    client.close()
    return json.loads(response)


stalled = connect()
stalled.sendall(b"{")
time.sleep(0.05)
started = time.monotonic()
probe = subprocess.run(
    [umbriel, "windows", "--json"],
    check=False,
    capture_output=True,
    text=True,
    timeout=2,
)
elapsed = time.monotonic() - started
if probe.returncode != 0:
    raise SystemExit(f"parallel IPC request failed: {probe.stderr.strip()}")
if elapsed >= 0.3:
    raise SystemExit(f"partial client delayed parallel IPC by {elapsed * 1000:.1f} ms")
if not isinstance(json.loads(probe.stdout), list):
    raise SystemExit("parallel windows response is not an array")

stalled.settimeout(2)
try:
    while stalled.recv(4096):
        pass
except socket.timeout:
    raise SystemExit("partial client exceeded the connection deadline")
finally:
    stalled.close()

if request(b"{}\n").get("err") != "malformed request":
    raise SystemExit("malformed request did not return its protocol error")
if request(b"x" * 65537).get("err") != "request too long":
    raise SystemExit("oversized request did not return its protocol error")

two = request(b'{"cmd":"windows"}\n{"cmd":"layers"}\n')
if "ok" not in two or not isinstance(two["ok"], list):
    raise SystemExit("one-request connection returned a malformed response")

# Unknown window ids must be rejected by both window actions with the same
# error, delivered through the msg action path.
for action in ("window-focus", "window-close"):
    reply = request(f'{{"cmd":"msg","arg":"{action}:definitely-not-a-window"}}\n'.encode())
    if reply.get("err") != "unknown window: definitely-not-a-window":
        raise SystemExit(f"{action} did not reject an unknown window id: {reply}")
PY

spawn_client
wait_for_windows 1

windows=$("$UMBRIEL" windows --json)
if ! jq -e 'type == "array"' <<< "$windows" > /dev/null; then
  echo "windows --json is not an array: $windows"
  exit 1
fi
# Every window entry carries the identity fields noctalia joins on, with the workspace id in the ext-workspace "<output>:<serial>" shape and a boolean
# seat-global active flag distinct from the per-workspace focused flag.
if ! jq -e '
  all(.[];
    (has("id") and (.id | type == "string"))
    and (has("workspace") and (.workspace | type == "string"))
    and (has("active") and (.active | type == "boolean"))
  )' <<< "$windows" > /dev/null; then
  echo "windows entries lack id/workspace/active: $windows"
  exit 1
fi
if ! jq -e '.[0].id != "" and (.[0].workspace | test("^HEADLESS-1:[0-9]+$"))' <<< "$windows" > /dev/null; then
  echo "windows id/workspace have an unexpected shape: $windows"
  exit 1
fi
if [[ $(jq -r '.[0].active' <<< "$windows") != $(jq -r '.[0].focused' <<< "$windows") ]]; then
  echo "active flag does not match the focused state of the only window: $windows"
  exit 1
fi

workspaces=$("$UMBRIEL" workspaces --json)
if ! jq -e '
  type == "array" and length >= 1
  and all(.[];
    (has("id") and (.id | type == "string"))
    and (has("name") and (.name | type == "string"))
    and (has("index") and (.index | type == "number") and .index >= 1)
    and (has("output") and (.output | type == "string"))
    and (has("active") and (.active | type == "boolean"))
    and (has("focused") and (.focused | type == "boolean"))
    and (has("layout") and (.layout | type == "string"))
  )
  and ([.[] | select(.active)] | length == 1)
  and ([.[] | select(.focused)] | length == 1)
  and all(.[] | select(.focused); .active)
  and all(.[]; .layout == "scrolling")
' <<< "$workspaces" > /dev/null; then
  echo "workspaces --json has an unexpected initial shape: $workspaces"
  exit 1
fi
if ! jq -e '
  .[0].index == 1
  and .[0].name == "1"
  and .[0].output == "HEADLESS-1"
  and (.[0].id | test("^HEADLESS-1:[0-9]+$"))
' <<< "$workspaces" > /dev/null; then
  echo "first workspace has unexpected identity fields: $workspaces"
  exit 1
fi

# The listing reports the effective mode, including a runtime override, rather
# than only the mode last loaded from configuration.
"$UMBRIEL" msg workspace-set-layout:dwindle > /dev/null
workspaces=$("$UMBRIEL" workspaces --json)
if ! jq -e '[.[] | select(.focused) | .layout] == ["dwindle"]' <<< "$workspaces" > /dev/null; then
  echo "focused workspace did not report its runtime layout override: $workspaces"
  exit 1
fi
workspace_human=$("$UMBRIEL" workspaces)
if ! grep -F "* HEADLESS-1: 1 [dwindle] (focused)" <<< "$workspace_human" > /dev/null; then
  echo "human workspace listing does not identify the focused layout: $workspace_human"
  exit 1
fi
"$UMBRIEL" msg workspace-set-layout:scrolling > /dev/null

# The workspaces event family carries the layout mode, which no Wayland protocol exposes, so a bar follows
# workspace-set-layout by subscribing instead of polling.
python3 - "$UMBRIEL_SOCKET" "$UMBRIEL" <<'PY'
import json
import selectors
import socket
import subprocess
import sys
import time

socket_path, umbriel = sys.argv[1:]


def connect():
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(socket_path)
    return client


def read_one(client, buf):
    client.settimeout(5)
    while b"\n" not in buf:
        chunk = client.recv(4096)
        if not chunk:
            return None, buf
        buf += chunk
    line, buf = buf.split(b"\n", 1)
    return line, buf


def action(*args):
    subprocess.run([umbriel, "msg", *args], check=True, capture_output=True, text=True, timeout=5)


def focused_layout(payload):
    focused = [ws for ws in payload["data"] if ws["focused"]]
    return focused[0]["layout"] if focused else None


sub = connect()
sub.sendall(b'{"cmd":"subscribe","events":["workspaces"]}\n')
buf = b""
line, buf = read_one(sub, buf)
if line is None:
    raise SystemExit("subscribing to workspaces delivered no initial state")
initial = json.loads(line)
if initial.get("event") != "workspaces" or not isinstance(initial.get("data"), list) or not initial["data"]:
    raise SystemExit(f"initial workspaces event has the wrong shape: {line!r}")
for key in ("id", "name", "index", "output", "active", "focused", "layout"):
    if key not in initial["data"][0]:
        raise SystemExit(f"workspaces event entry lacks '{key}': {initial['data'][0]}")
if focused_layout(initial) != "scrolling":
    raise SystemExit(f"initial workspaces event does not report the active layout: {initial['data']}")

# A runtime layout switch must be pushed, and the pushed payload must be the effective mode.
action("workspace-set-layout:master")
pushed = None
deadline = time.monotonic() + 5
while time.monotonic() < deadline:
    line, buf = read_one(sub, buf)
    if line is None:
        break
    parsed = json.loads(line)
    if parsed.get("event") == "workspaces" and focused_layout(parsed) == "master":
        pushed = parsed
        break
if pushed is None:
    raise SystemExit("workspace-set-layout did not push a workspaces event")
sub.close()
action("workspace-set-layout:scrolling")

# The CLI verb relays the same stream, one flushed line per event, so a script can read it live.
stream = subprocess.Popen(
    [umbriel, "subscribe", "workspaces"],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)
selector = selectors.DefaultSelector()
selector.register(stream.stdout, selectors.EVENT_READ)


def relayed_line(seconds=5):
    return stream.stdout.readline() if selector.select(seconds) else ""


try:
    first = relayed_line()
    if not first.strip():
        raise SystemExit("umbriel subscribe printed no initial line")
    if json.loads(first).get("event") != "workspaces":
        raise SystemExit(f"umbriel subscribe printed a foreign line: {first!r}")
    action("workspace-set-layout:dwindle")
    relayed = None
    for _ in range(10):
        line = relayed_line()
        if not line.strip():
            break
        if focused_layout(json.loads(line)) == "dwindle":
            relayed = line
            break
    if relayed is None:
        raise SystemExit("umbriel subscribe did not relay the pushed event")
finally:
    selector.close()
    stream.terminate()
    stream.wait(timeout=5)
action("workspace-set-layout:scrolling")

# Submap subscribers receive the current context immediately, then each change
# to the active value. A reset of a nested layer reveals its parent before the
# default context returns as null.
sub = connect()
sub.sendall(b'{"cmd":"subscribe","events":["submap"]}\n')
buf = b""


def expect_submap(want, reason):
    global buf
    line, buf = read_one(sub, buf)
    if line is None:
        raise SystemExit(f"{reason} delivered no submap event")
    event = json.loads(line)
    if event.get("event") != "submap" or event.get("data") != want:
        raise SystemExit(f"{reason} delivered the wrong submap event: {event!r}")


expect_submap(None, "subscribing")
action("submap:outer")
expect_submap("outer", "entering the outer layer")
action("submap:inner")
expect_submap("inner", "entering the inner layer")
action("submap:reset")
expect_submap("outer", "resetting the inner layer")
action("submap:reset")
expect_submap(None, "resetting the outer layer")
sub.close()

# An unknown family is rejected by name, and the client exits instead of waiting on a stream that will never open.
rejected = subprocess.run([umbriel, "subscribe", "definitely-not-an-event"], capture_output=True, text=True, timeout=5)
if rejected.returncode == 0:
    raise SystemExit("umbriel subscribe accepted an unknown event")
if "unknown subscription event: definitely-not-an-event" not in rejected.stderr:
    raise SystemExit(f"umbriel subscribe did not name the unknown event: {rejected.stderr!r}")
PY

submap=$("$UMBRIEL" submap --json)
if ! jq -e '. == null' <<< "$submap" > /dev/null; then
  echo "submap --json did not report the default context: $submap"
  exit 1
fi
if [[ $("$UMBRIEL" submap | wc -c) -ne 0 ]]; then
  echo "human submap output printed text for the default context"
  exit 1
fi

# Submaps nest, so current means the top layer that handles keybinds. Popping
# that layer must reveal the previous one before returning to the default map.
"$UMBRIEL" msg submap:outer > /dev/null
submap=$("$UMBRIEL" submap --json)
if ! jq -e '. == "outer"' <<< "$submap" > /dev/null; then
  echo "submap --json did not report the active outer layer: $submap"
  exit 1
fi
if [[ $("$UMBRIEL" submap) != "outer" ]]; then
  echo "human submap output did not report the active outer layer"
  exit 1
fi
"$UMBRIEL" msg submap:inner > /dev/null
submap=$("$UMBRIEL" submap --json)
if ! jq -e '. == "inner"' <<< "$submap" > /dev/null; then
  echo "submap --json did not report the nested top layer: $submap"
  exit 1
fi
"$UMBRIEL" msg submap:reset > /dev/null
submap=$("$UMBRIEL" submap --json)
if ! jq -e '. == "outer"' <<< "$submap" > /dev/null; then
  echo "submap reset did not reveal the previous layer: $submap"
  exit 1
fi
"$UMBRIEL" msg submap:reset > /dev/null
submap=$("$UMBRIEL" submap --json)
if ! jq -e '. == null' <<< "$submap" > /dev/null; then
  echo "submap reset did not restore the default context: $submap"
  exit 1
fi

layers=$("$UMBRIEL" layers --json)
if ! jq -e 'type == "array"' <<< "$layers" > /dev/null; then
  echo "layers --json is not an array: $layers"
  exit 1
fi

# Output management is a Wayland-client query rather than IPC, but it follows
# the same JSON flag convention as the inspection commands.
outputs=$("$UMBRIEL" outputs --json)
if ! jq -e '
  type == "array" and length == 1
  and all(.[];
    (has("name") and (.name | type == "string"))
    and (has("description") and (.description | type == "string"))
    and (has("make") and (.make | type == "string"))
    and (has("model") and (.model | type == "string"))
    and (has("serial") and (.serial | type == "string"))
    and (has("config_name") and (.config_name == null or (.config_name | type == "string")))
    and (has("physical_size") and (.physical_size | type == "object")
      and (.physical_size.width_mm | type == "number")
      and (.physical_size.height_mm | type == "number"))
    and (has("enabled") and (.enabled | type == "boolean"))
    and (has("position") and (.position | type == "object")
      and (.position.x | type == "number")
      and (.position.y | type == "number"))
    and (has("transform") and (.transform | type == "string"))
    and (has("scale") and (.scale | type == "number"))
    and (has("adaptive_sync") and (.adaptive_sync == null or (.adaptive_sync | type == "boolean")))
    and (has("modes") and (.modes | type == "array")
      and all(.modes[];
        (.width | type == "number")
        and (.height | type == "number")
        and (.refresh_mhz | type == "number")
        and (.preferred | type == "boolean")
        and (.current | type == "boolean")))
  )
' <<< "$outputs" > /dev/null; then
  echo "outputs --json has an unexpected shape: $outputs"
  exit 1
fi
if [[ $("$UMBRIEL" outputs -j) != "$outputs" ]]; then
  echo "outputs -j differs from outputs --json"
  exit 1
fi

# The headless harness may have no physical keyboard, in which case the command
# errors with "no keyboard". Exactly one of the two shapes must appear.
if layouts=$("$UMBRIEL" keyboard-layouts --json 2>/dev/null); then
  if ! jq -e '
    type == "object"
    and (has("names") and (.names | type == "array"))
    and (has("current_index") and (.current_index | type == "number"))
  ' <<< "$layouts" > /dev/null; then
    echo "keyboard-layouts --json has an unexpected shape: $layouts"
    exit 1
  fi
else
  err=$("$UMBRIEL" keyboard-layouts 2>&1 >/dev/null) || true
  if [[ $err != *"no keyboard"* ]]; then
    echo "keyboard-layouts neither returned layouts nor the no-keyboard error"
    exit 1
  fi
fi

# An unknown action must be rejected, not silently accepted.
if "$UMBRIEL" msg definitely-not-an-action > /dev/null 2>&1; then
  echo "msg accepted an unknown action"
  exit 1
fi

python3 - "$UMBRIEL_SOCKET" "$UMBRIEL" <<'PY'
import json
import socket
import subprocess
import sys
import time

socket_path, umbriel = sys.argv[1:]


def connect():
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(socket_path)
    return client


def request(payload):
    client = connect()
    client.sendall(payload)
    response = b""
    while True:
        chunk = client.recv(4096)
        if not chunk:
            break
        response += chunk
    client.close()
    return json.loads(response)


def read_one(client, buf):
    client.settimeout(5)
    while b"\n" not in buf:
        chunk = client.recv(4096)
        if not chunk:
            return None, buf
        buf += chunk
    line, buf = buf.split(b"\n", 1)
    return line, buf


# The subscribed connection stays open and first delivers the initial state of
# every subscribed event, in the fixed order overview, windows.
sub = connect()
sub.sendall(b'{"cmd":"subscribe","events":["overview","windows"]}\n')
buf = b""
initial = []
for _ in range(2):
    line, buf = read_one(sub, buf)
    if line is None:
        break
    initial.append(line)
if len(initial) != 2:
    raise SystemExit(f"subscribe returned {len(initial)} initial line(s), expected 2: {initial!r}")

first = json.loads(initial[0])
second = json.loads(initial[1])
if first.get("event") != "overview" or not isinstance(first.get("data", {}).get("open"), bool):
    raise SystemExit(f"first initial event is not an overview event: {initial[0]}")
if second.get("event") != "windows" or not isinstance(second.get("data"), list):
    raise SystemExit(f"second initial event is not a windows event: {initial[1]}")
if not second["data"]:
    raise SystemExit("windows initial event has no windows")
for key in ("id", "workspace", "active"):
    if key not in second["data"][0]:
        raise SystemExit(f"windows event entry lacks '{key}': {second['data'][0]}")
real_id = second["data"][0]["id"]
if not real_id:
    raise SystemExit("windows event entry has an empty id")

# The focus round trip resolves the exact identifier; the same lookup must
# also close the window.
focused = request(f'{{"cmd":"msg","arg":"window-focus:{real_id}"}}\n'.encode())
if "err" in focused or "ok" not in focused:
    raise SystemExit(f"window-focus on the real window failed: {focused}")

# Other actions can push windows events (focus changes schedule them), so skip
# every line until the pushed overview event arrives.
subprocess.run([umbriel, "msg", "overview-open"], check=True, capture_output=True, text=True, timeout=5)
event = None
deadline = time.monotonic() + 5
while time.monotonic() < deadline:
    line, buf = read_one(sub, buf)
    if line is None:
        break
    parsed = json.loads(line)
    if parsed.get("event") == "overview":
        event = parsed
        break
if event is None or event.get("data", {}).get("open") is not True:
    raise SystemExit(f"overview-open did not push an open event: {event!r}")
sub.close()
subprocess.run([umbriel, "msg", "overview-close"], check=True, capture_output=True, text=True, timeout=5)

closed = request(f'{{"cmd":"msg","arg":"window-close:{real_id}"}}\n'.encode())
if "err" in closed or "ok" not in closed:
    raise SystemExit(f"window-close on the real window failed: {closed}")
PY

# An "ok" reply only says the request was accepted. The window must actually
# leave the list, which is the close path itself, not tidying up after it.
wait_for_windows 0
echo "IPC commands return documented JSON, human-readable listings, events, and clean window lifecycle replies"
