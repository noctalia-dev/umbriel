#!/usr/bin/env bash
# Killing a window terminates its client with SIGKILL, including clients that ignore close requests.
set -euo pipefail

python3 - <<'PY'
import json
import os
import signal
import subprocess
import time

umbriel = os.environ["UMBRIEL"]
client = os.environ["UMBRIEL_SEAT_LOG_CLIENT"]


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
print("focused scratchpad and explicit background targets exited with SIGKILL; other clients survived")
PY
