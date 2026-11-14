#!/usr/bin/env bash
# harness: xwayland=true
# Killing a window terminates its native client or X11 connection, including retained X11 resources.
set -euo pipefail

python3 - <<'PY'
import json
import os
import signal
import subprocess
import time

umbriel = os.environ["UMBRIEL"]
client = os.environ["UMBRIEL_SEAT_LOG_CLIENT"]
x11_client = os.environ["UMBRIEL_XWAYLAND_ASSOCIATION_CLIENT"]


def action(*words, success=True):
    result = subprocess.run([umbriel, "msg", *words], capture_output=True, text=True, timeout=5)
    if (result.returncode == 0) != success:
        raise SystemExit(f"unexpected result for {' '.join(words)}: {result.stdout} {result.stderr}")


def windows():
    result = subprocess.run([umbriel, "windows", "--json"], capture_output=True, text=True, check=True, timeout=5)
    return json.loads(result.stdout)


def await_windows(predicate, description):
    for _ in range(100):
        state = windows()
        if predicate(state):
            return state
        time.sleep(0.025)
    raise SystemExit(f"timed out waiting for {description}: {state}")


def spawn(title):
    log = open(os.path.join(os.environ["UMBRIEL_RUNTIME_DIR"], title + ".log"), "w")
    process = subprocess.Popen([client, title], stdout=log, stderr=subprocess.STDOUT)
    log.close()
    state = await_windows(lambda state: any(w["pid"] == process.pid and w["active"] for w in state), title)
    return process, next(w["id"] for w in state if w["pid"] == process.pid)


def assert_killed(process):
    try:
        status = process.wait(timeout=4)
    except subprocess.TimeoutExpired:
        raise SystemExit("window-kill left the client process running")
    if status != -signal.SIGKILL:
        raise SystemExit(f"expected SIGKILL, got process status {status}")
    await_windows(lambda state: all(w["pid"] != process.pid for w in state), "killed window to unmap")


def spawn_x11(title, *commands):
    log_path = os.path.join(os.environ["UMBRIEL_RUNTIME_DIR"], title + ".log")
    with open(log_path, "w") as log:
        process = subprocess.Popen(
            [x11_client, title, "479", "353", "managed"],
            stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, text=True,
        )
    process.stdin.write("\n".join((*commands, "map")) + "\n")
    process.stdin.flush()
    state = await_windows(lambda state: any(w["title"] == title and w["xwayland"] for w in state), title)
    return process, next(w["id"] for w in state if w["title"] == title)


def await_x11_removed(process, window_id):
    await_windows(lambda state: all(w["id"] != window_id for w in state), "killed X11 window to unmap")
    try:
        status = process.wait(timeout=4)
    except subprocess.TimeoutExpired:
        raise SystemExit("window-kill left the X11 connection open")
    if status not in (0, -signal.SIGKILL):
        raise SystemExit(f"X11 connection shutdown returned {status}")


# Like window-close, an empty desktop is a successful no-op.
action("window-kill")
background, background_id = spawn("kill-background")
focused, focused_id = spawn("kill-focused")
action("window-kill", "definitely-not-a-window", success=False)
if background.poll() is not None or focused.poll() is not None:
    raise SystemExit("an unknown window ID terminated a client")

# The bare action must use seat focus, including when a scratchpad covers the workspace focus.
action("window-move-to-scratchpad")
action("scratchpad-toggle")
await_windows(lambda state: any(w["id"] == focused_id and w["active"] for w in state), "scratchpad focus")
action("window-kill")
assert_killed(focused)
if background.poll() is not None:
    raise SystemExit("killing the focused scratchpad also terminated the background client")

# An explicit ID selects a background window without killing the focused client.
survivor, survivor_id = spawn("kill-survivor")
action("window-kill", background_id)
assert_killed(background)
if survivor.poll() is not None:
    raise SystemExit("killing a window by ID also terminated the focused client")
await_windows(lambda state: len(state) == 1 and state[0]["id"] == survivor_id and state[0]["active"], "survivor focus")
action("window-kill")
assert_killed(survivor)
action("window-kill")

# Keep another X11 client alive so lazy server shutdown cannot remove a stale target for us.
x11_survivor, x11_survivor_id = spawn_x11("kill-x11-survivor")
for title, commands in (
    ("kill-x11-live", ()),
    ("kill-x11-ignores-close", ("delete-protocol", "retain")),
):
    process, window_id = spawn_x11(title, *commands)
    if commands:
        action("window-close", window_id)
        if process.poll() is not None or not any(w["id"] == window_id for w in windows()):
            raise SystemExit("the X11 client did not ignore the graceful close request")
    action("window-kill", window_id)
    await_x11_removed(process, window_id)

# A crashed owner may leave a mapped X11 resource whose saved PID has already been reaped.
retained, retained_id = spawn_x11("kill-x11-retained", "delete-protocol", "retain")
await_windows(
    lambda state: any(w["id"] == retained_id and w["pid"] == retained.pid for w in state),
    "retained X11 window owner PID",
)
retained.kill()
if retained.wait(timeout=4) != -signal.SIGKILL:
    raise SystemExit("the retained X11 owner did not die from SIGKILL")
await_windows(lambda state: any(w["id"] == retained_id for w in state), "X11 resource after its owner exited")
action("window-close", retained_id)
if not any(w["id"] == retained_id for w in windows()):
    raise SystemExit("a graceful close removed an ownerless retained X11 window")
action("window-kill", retained_id)
await_windows(lambda state: all(w["id"] != retained_id for w in state), "retained X11 resource to disappear")
if x11_survivor.poll() is not None:
    raise SystemExit("killing an X11 window terminated an unrelated X11 client")
await_windows(
    lambda state: len(state) == 1 and state[0]["id"] == x11_survivor_id,
    "unrelated X11 window to survive",
)
print("native clients die from SIGKILL; live and ownerless retained X11 windows are removed without affecting another client")
PY
