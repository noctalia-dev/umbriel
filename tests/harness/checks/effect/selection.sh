#!/usr/bin/env bash
# Stable owner-local pool assignments, first-map inspection and pixels, runtime
# transitions, focus/title history, mapped lifetimes, and effect-only events.
set -euo pipefail
cat > "$UMBRIEL_RUNTIME_DIR/red.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/green.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/ring.glsl" <<'GLSL'
vec4 border(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'TOML'

[animation]
enabled = false
[appearance]
border_width = 6
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[effects]
window = "colors"
border = "rings"
in_capture = true
[effects.preset.red]
kind = "window"
shader = "red.glsl"
[effects.preset.green]
kind = "window"
shader = "green.glsl"
[effects.preset.ring]
kind = "border"
shader = "ring.glsl"
[effects.pool.colors]
kind = "window"
choose = ["red", "green"]
[effects.pool.focused]
kind = "window"
choose = ["green", "red"]
selection = "round_robin"
[effects.pool.rings]
kind = "border"
choose = ["ring"]
[effects.pool.empty]
kind = "window"
choose = []
[[window_rule]]
match.title = "^pool-one$"
default_floating = true
default_position = { x = 70, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^pool-two$"
default_floating = true
default_position = { x = 460, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^pool-three$"
default_floating = true
default_position = { x = 850, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^pool-history$"
default_floating = true
default_position = { x = 460, y = 410, anchor = "top_left" }
window_effect = "off"
[[window_rule]]
match.title = "^pool-history$"
match.is_focused = true
window_effect = "focused"
[[window_rule]]
match.title = "^pool-changed$"
window_effect = "red"
TOML
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" clock-freeze > /dev/null

python3 - <<'PY'
import json
import os
import socket
import subprocess
import time

umbriel = os.environ["UMBRIEL"]
image = os.path.join(os.environ["UMBRIEL_RUNTIME_DIR"], "selection.png")
clients = []

def run(*args):
    return subprocess.check_output([umbriel, *args], text=True, timeout=10)

def action(name, argument=None):
    run("msg", name if argument is None else f"{name}:{argument}")

def settle():
    run("settle")

def windows():
    return json.loads(run("windows", "--json"))

def effects():
    return json.loads(run("effects", "--json"))

def expect(label, actual, wanted):
    if actual != wanted:
        raise SystemExit(f"{label}: got {actual!r}, expected {wanted!r}")

def window(identifier):
    return next(w for w in windows() if w["id"] == identifier)

def slot(identifier):
    return window(identifier)["window_effect"]

def selected(label, identifier, name, pool="colors", source="default", suppressed=False):
    expect(label, slot(identifier), dict(name=name, pool=pool, source=source, suppressed=suppressed))

def held(label, counts):
    actual = next(p for p in effects()["pools"] if p["name"] == "colors")
    expect(label, [m["held"] for m in actual["members"]], counts)

def pixels(label, identifier, color, border=False):
    settle()
    w = window(identifier)
    subprocess.run(["grim", image], check=True, timeout=10)
    x = int(w["x"] + w["w"] / 2)
    y = int(w["y"] + w["h"] + 3 if border else w["y"] + w["h"] - 20)
    values = subprocess.check_output([os.environ["UMBRIEL_PIXEL_PROBE"], image, "pixel", str(x), str(y)], text=True)
    rgb = tuple(map(int, values.split()))
    target = {"red": (255, 0, 0), "green": (0, 255, 0), "blue": (0, 0, 255)}[color]
    if any(abs(value - wanted) > 20 for value, wanted in zip(rgb, target)):
        raise SystemExit(f"{label}: pixel {rgb}, expected {color}")

sub = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sub.connect(os.environ["UMBRIEL_SOCKET"])
sub.sendall(b'{"cmd":"subscribe","events":["windows"]}\n')
sub.settimeout(10)
stream = sub.makefile("rb")
json.loads(stream.readline())

def event_window(title):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        event = json.loads(stream.readline())
        for w in event.get("data", []):
            if w["title"] == title:
                return w
    raise SystemExit(f"first-map-event: no event for {title}")

def spawn(title, expected):
    env = dict(os.environ, FILL_COLOR="0xFF0000FF", REMAP_ON_STDIN="1")
    if title == "pool-history":
        env["TITLE_AFTER_MAP"] = "pool-changed"
    log = open(os.path.join(os.environ["UMBRIEL_RUNTIME_DIR"], title + ".log"), "w")
    child = subprocess.Popen([os.environ["UMBRIEL_UNMAP_CLIENT"], title, "260", "180"],
                             env=env, stdin=subprocess.PIPE, stdout=log, stderr=log)
    clients.append(child)
    first = event_window(title)
    expect("first-map-assignment", first["window_effect"]["name"], expected)
    pixels("first-visible-pixels", first["id"], expected)
    return first["id"], child

one, first_client = spawn("pool-one", "red")
two, _ = spawn("pool-two", "green")
three, _ = spawn("pool-three", "red")
held("three-owner-holdings", [2, 1])
# Inspection cannot consume policy state or change assignments, and the windows
# subscription shares the same slot shape as the direct query.
before = effects()
for _ in range(3):
    expect("inspection-purity", effects(), before)
expect("declaration-order", [p["name"] for p in before["presets"]], ["red", "green", "ring"])
expect("pool-declaration-order", [p["name"] for p in before["pools"]], ["colors", "focused", "rings", "empty"])
# Unfocused borders continue to hold the one-member border pool.
ring_pool = next(p for p in before["pools"] if p["name"] == "rings")
expect("unfocused-border-holdings", ring_pool["members"][0]["held"], 3)
pixels("first-border-pixels", three, "red", border=True)

# Subscribe afresh to eliminate unrelated map/focus events before the action.
sub.close()
stream.close()
sub = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sub.connect(os.environ["UMBRIEL_SOCKET"])
sub.sendall(b'{"cmd":"subscribe","events":["windows"]}\n')
sub.settimeout(10)
stream = sub.makefile("rb")
json.loads(stream.readline())
action("effect-window-cycle", "/" + one)
event = event_window("pool-one")
expect("effect-only-event", event["window_effect"], dict(name="green", pool="colors", source="runtime", suppressed=False))
selected("cycle-override", one, "green", source="runtime")
pixels("cycle-pixels", one, "green")
held("cycle-holdings", [1, 2])
action("effect-window-set", "off/" + one)
selected("off-keeps-assignment", one, "green", source="runtime", suppressed=True)
pixels("off-pixels", one, "blue")
held("suppression-releases-holding", [1, 1])
action("effect-window-set", "off/" + one)
selected("repeated-off", one, "green", source="runtime", suppressed=True)
action("effect-window-toggle", one)
selected("toggle-restores-assignment", one, "green", source="runtime")
pixels("toggle-pixels", one, "green")
action("effect-window-reset", one)
selected("reset-configured-selection", one, "red")
pixels("reset-pixels", one, "red")
action("effect-window-set", "green/" + one)
selected("plain-runtime-selection", one, "green", pool="", source="runtime")
pixels("plain-runtime-pixels", one, "green")
held("plain-preset-does-not-hold", [1, 1])

# Each rejected request is checked against the whole effects response, including
# holdings, assignments and suppression. The next valid pick is checked below.
for rejection, (command, argument, message) in enumerate([
    ("effect-window-set", "missing/" + one, "unknown"),
    ("effect-window-set", "ring/" + one, "window"),
    ("effect-window-set", "red/does-not-exist", "unknown window"),
    ("effect-window-cycle", "red/" + one, "pool"),
    ("effect-window-cycle", "empty/" + one, "pool is empty"),
    ("effect-window-cycle", "/" + one, "no pool to cycle"),
]):
    before = effects()
    result = subprocess.run([umbriel, "msg", f"{command}:{argument}"], capture_output=True, text=True, timeout=10)
    if result.returncode == 0 or message not in result.stderr:
        raise SystemExit(f"transaction-error-{rejection}: {command}:{argument}: {result.returncode}, {result.stderr}")
    expect(f"transaction-state-{rejection}", effects(), before)

action("effect-window-set", "empty/" + one)
selected("empty-pool-selection", one, "", pool="empty", source="runtime")
pixels("empty-pool-pixels", one, "blue")
action("effect-window-toggle", one)
selected("empty-toggle-noop", one, "", pool="empty", source="runtime")
action("effect-window-reset", one)
selected("post-error-policy-pick", one, "red")

history, history_client = spawn("pool-history", "green")
selected("focused-rule-source", history, "green", pool="focused", source="rule")
action("window-focus", one)
selected("unfocused-rule-off", history, "", pool="", source="rule")
pixels("unfocused-rule-pixels", history, "blue")
action("window-focus", history)
selected("focus-history-restored", history, "green", pool="focused", source="rule")
pixels("focus-history-pixels", history, "green")
history_client.stdin.write(b"t")
history_client.stdin.flush()
for _ in range(200):
    if window(history)["title"] == "pool-changed":
        break
    time.sleep(0.01)
selected("title-rule-selection", history, "red", pool="", source="rule")
pixels("title-rule-pixels", history, "red")

# Unmap releases both holdings while retaining the client's View object. Remap
# must clear runtime overrides, suppression, and pool history from that object.
action("effect-window-set", "green/" + one)
action("effect-window-set", "off/" + one)
action("window-close", one)
for _ in range(200):
    if all(w["id"] != one for w in windows()):
        break
    time.sleep(0.01)
else:
    raise SystemExit("unmap-release: client did not unmap")
held("unmap-release", [1, 1])
first_client.stdin.write(b"r")
first_client.stdin.flush()
for _ in range(200):
    matches = [w for w in windows() if w["title"] == "pool-one"]
    if matches:
        one = matches[0]["id"]
        break
    time.sleep(0.01)
else:
    raise SystemExit("remap-state: client did not remap")
selected("remap-state", one, "red")
pixels("remap-pixels", one, "red")
held("remap-holdings", [2, 1])
# Keep the output physically asleep while layout changes, so no frame can
# resolve a pending alone rule on behalf of the action or inspection request.
with open(os.environ["UMBRIEL_CONFIG"], "a") as config:
    config.write(r'''
[effects.pool.inspection_alone]
kind = "window"
choose = ["red"]
[effects.pool.inspection_shared]
kind = "window"
choose = ["green"]
[[window_rule]]
match.title = "^inspect-"
default_floating = false
window_effect = "inspection_shared"
[[window_rule]]
match.title = "^inspect-"
match.is_alone = true
window_effect = "inspection_alone"
''')
action("config-reload")
action("workspace-switch", "2")
inspect_one, _ = spawn("inspect-one", "red")
inspect_two, _ = spawn("inspect-two", "green")
selected("tiled-shared-selection", inspect_one, "green", pool="inspection_shared", source="rule")
action("dpms-off")
action("window-toggle-floating", inspect_two)
before = effects()
inspected = next(owner["slots"]["window"] for owner in before["owners"] if owner.get("id") == inspect_one)
expect("alone-before-frame-or-inspection", inspected,
       dict(name="red", pool="inspection_alone", source="rule", suppressed=False))
for _ in range(3):
    listing = windows()
    expect("pending-layout-inspection-purity", effects(), before)
preview = next(w for w in listing if w["id"] == inspect_one)
action("dpms-on")
settle()
arranged = window(inspect_one)
expect("pending-layout-preview-geometry", [preview["x"], preview["y"]], [arranged["x"], arranged["y"]])
pixels("alone-after-wake-pixels", inspect_one, "red")
action("dpms-off")
action("window-toggle-floating", inspect_two)
before = effects()
inspected = next(owner["slots"]["window"] for owner in before["owners"] if owner.get("id") == inspect_one)
expect("shared-before-frame-or-inspection", inspected,
       dict(name="green", pool="inspection_shared", source="rule", suppressed=False))
windows()
expect("retiled-inspection-purity", effects(), before)
action("dpms-on")
pixels("shared-after-wake-pixels", inspect_one, "green")
# Closing an animated overview can restore focus from an output-frame callback.
# A window first focused there has no remembered pool member: selection must run
# in its owner-local idle, with settle waiting for that selection and its frame.
config_path = os.environ["UMBRIEL_CONFIG"]
with open(config_path) as config:
    text = config.read().replace("[animation]\nenabled = false", "[animation]\nenabled = true", 1)
with open(config_path, "w") as config:
    config.write(text)
    config.write(r'''
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
[animation.windows_move]
enabled = false
[animation.border]
enabled = false
[animation.dim_unfocused]
enabled = false
[animation.overview]
duration_ms = 100
[effects.pool.inspection_focus]
kind = "window"
choose = ["red"]
[[window_rule]]
match.title = "^inspect-focus$"
default_floating = true
default_focused = false
window_effect = "off"
[[window_rule]]
match.title = "^inspect-focus$"
match.is_focused = true
window_effect = "inspection_focus"
''')
action("config-reload")
action("workspace-switch", "3")
action("overview-open")
run("clock-advance", "1000")
log = open(os.path.join(os.environ["UMBRIEL_RUNTIME_DIR"], "inspect-focus.log"), "w")
focus_client = subprocess.Popen([os.environ["UMBRIEL_UNMAP_CLIENT"], "inspect-focus", "260", "180"],
                                env=dict(os.environ, FILL_COLOR="0xFF0000FF"), stdout=log, stderr=log)
clients.append(focus_client)
first = event_window("inspect-focus")
expect("overview-unfocused-initial-slot", first["window_effect"]["name"], "")
action("overview-close")
run("clock-advance", "1000")
settle()
selected("overview-frame-deferred-selection", first["id"], "red", pool="inspection_focus", source="rule")
pixels("overview-frame-deferred-pixels", first["id"], "red")
print("pool first-map assignments, pixels, actions, events, history, remap and deferred selection/inspection verified")
PY
