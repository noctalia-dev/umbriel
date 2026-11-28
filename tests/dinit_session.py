#!/usr/bin/env python3
"""Exercise the session launcher and compositor with a private user manager and bus."""
import json
import os
from pathlib import Path
import shutil
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

HELPER = r'''#!PYTHON
import json, os, signal, socket, subprocess, sys, time
from pathlib import Path
root = Path(os.environ["UMBRIEL_DINIT_TEST_ROOT"])
phase = os.environ.get("UMBRIEL_DINIT_TEST_PHASE", "normal")
role = sys.argv[1] if Path(sys.argv[0]).name != "dbus-update-activation-environment" else "import"
assert "UMBRIEL_DINIT_READY_FD" not in os.environ
if role == "import":
    if os.environ.get("UMBRIEL_SOCKET") and (root / "gate-enabled").exists():
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as gate:
            gate.connect(str(root / "gate"))
            gate.sendall(b"waiting")
            if gate.recv(16) != b"release":
                sys.exit(1)
    os.execv(os.environ["UMBRIEL_DINIT_TEST_DBUS_UPDATE"],
             [os.environ["UMBRIEL_DINIT_TEST_DBUS_UPDATE"], *sys.argv[1:]])
keys = ("WAYLAND_DISPLAY", "DISPLAY", "UMBRIEL_SOCKET", "PATH", "DINIT_SOCKET_PATH",
        "UMBRIEL_DINIT_TEST_VALUE", "UMBRIEL_LOGIN_PROFILE_MARKER")
record = {name: os.environ[name] for name in keys if name in os.environ}
record["pid"] = os.getpid()
record["arguments"] = sys.argv[2:]
def publish(name, text):
    temporary = root / (name + ".tmp." + str(os.getpid()))
    temporary.write_text(text)
    temporary.replace(root / name)
if role == "service":
    subprocess.run([os.environ["UMBRIEL_DINIT_TEST_BINARY"], "windows", "--json"],
                   check=True, stdout=subprocess.DEVNULL)
def stop(signum, frame):
    alive = subprocess.run([os.environ["UMBRIEL_DINIT_TEST_BINARY"], "windows", "--json"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    publish(phase + "-" + role + "-stopped", str(alive))
    sys.exit(0)
if role in ("service", "app"):
    with (root / "pids").open("a") as output:
        output.write(str(os.getpid()) + "\n")
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
publish(phase + "-" + role + ".json", json.dumps(record))
if role not in ("service", "app"):
    sys.exit(0)
while True:
    signal.pause()
'''


def wait_for(predicate, label, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.025)
    raise AssertionError("timed out: " + label)


def main():
    binary = Path(sys.argv[1]).resolve()
    repo = Path(__file__).resolve().parent.parent
    controller = shutil.which("dinitctl")
    daemon = shutil.which("dinit")
    assert controller and daemon, "dinit and dinitctl are required"
    updater = shutil.which("dbus-update-activation-environment")
    assert updater and shutil.which("dbus-daemon"), "D-Bus tools are required"
    with tempfile.TemporaryDirectory(prefix="um-dinit.") as directory:
        root = Path(directory)
        for name in ("bin", "home", "config", "cache", "services", "bus-services"):
            (root / name).mkdir()
        helper = root / "helper"
        helper.write_text(HELPER.replace("#!PYTHON", "#!" + sys.executable, 1))
        helper.chmod(0o700)
        (root / "bin" / "dbus-update-activation-environment").symlink_to(helper)
        (root / "bin" / "systemctl").write_text("#!/bin/sh\nexit 1\n")
        (root / "bin" / "systemctl").chmod(0o700)
        controller_proxy = root / "bin" / "dinitctl"
        controller_proxy.write_text(
            "#!/bin/sh\n"
            'if [ -n "${UMBRIEL_SOCKET:-}" ] && [ -e "$UMBRIEL_DINIT_TEST_ROOT/fail-manager" ]; then exit 1; fi\n'
            f'exec "{controller}" "$@"\n')
        controller_proxy.chmod(0o700)
        launcher = root / "start-umbriel"
        launcher.write_text((repo / "data/start-umbriel.in").read_text()
                            .replace("@UMBRIEL_EXECUTABLE@", str(binary))
                            .replace("/etc/shells", str(root / "shells")))
        launcher.chmod(0o700)
        login_shell = shutil.which("bash")
        (root / "shells").write_text(login_shell + "\n")
        config = root / "config.toml"
        literal = "quotes ' \" and a newline\n$(touch " + str(root / "should-not-exist") + ")"
        config.write_text(
            '[general]\nxwayland = false\nshow_cheatsheet = false\n'
            f'autostart = [{json.dumps(str(helper) + " app")}]\n'
            '[environment]\n'
            f'UMBRIEL_DINIT_TEST_VALUE = {json.dumps(literal)}\n'
            'PATH = "/configured/application/path"\n'
            'DINIT_SOCKET_PATH = "/configured/application/socket"\n')
        services = root / "services"
        (services / "boot").write_text("type = internal\n")
        (services / "dbus").write_text("type = internal\n")
        service = (repo / "data/dinit/umbriel.in").read_text().replace(
            '"@UMBRIEL_EXECUTABLE@"', f'"{binary}" -c "{config}"')
        (services / "umbriel").write_text(service)
        # A dependent service must see the published environment and a live IPC socket.
        (services / "component").write_text(
            f'type = process\ncommand = {helper} service\nrestart = false\n'
            'depends-on: umbriel\nstop-timeout = 2\n')
        target = (repo / "data/dinit/umbriel-session").read_text()
        (services / "umbriel-session").write_text(target)
        enabled = root / "home/.config/dinit.d/umbriel-session.d"
        enabled.mkdir(parents=True)
        (services / "inspect").write_text(f'type = scripted\ncommand = {helper} inspect\n')
        bus_config = root / "bus.conf"
        bus_config.write_text(
            '<busconfig><type>session</type><listen>unix:path=' + str(root / "bus") + '</listen>'
            '<auth>EXTERNAL</auth><servicedir>' + str(root / "bus-services") + '</servicedir>'
            '<policy context="default"><allow own="*"/><allow send_destination="*"/>'
            '<allow receive_sender="*"/></policy></busconfig>')
        bus_probes = ("normal_ready", "normal_stopped", "crash_stopped", "signal_stopped",
                      "failed_manager_stopped", "failed_import_stopped", "interrupted_start_stopped")
        for label in bus_probes:
            name = "org.umbriel.SessionProbe." + label
            (root / ("bus-services/" + name + ".service")).write_text(
                f'[D-BUS Service]\nName={name}\nExec={helper} bus-{label}\n')
        bus_sender = shutil.which("dbus-send")
        assert bus_sender, "dbus-send is required"
        runtime_variables = (
            "LD_LIBRARY_PATH", "LIBGL_DRIVERS_PATH", "GBM_BACKENDS_PATH", "__EGL_VENDOR_LIBRARY_FILENAMES",
            "MESA_LOADER_DRIVER_OVERRIDE", "WLR_RENDERER", "EGL_PLATFORM", "ASAN_OPTIONS", "LSAN_OPTIONS",
        )
        environment = {name: os.environ[name] for name in runtime_variables if name in os.environ}
        command_directories = list(dict.fromkeys(
            str(Path(shutil.which(name)).parent)
            for name in ("awk", "xargs", "sleep", "bash", "dbus-daemon")))
        environment.update(
            HOME=str(root / "home"), SHELL=login_shell,
            XDG_RUNTIME_DIR=str(root), XDG_CONFIG_HOME=str(root / "config"),
            XDG_CACHE_HOME=str(root / "cache"),
            DINIT_SOCKET_PATH=str(root / "control"),
            DBUS_SESSION_BUS_ADDRESS="unix:path=" + str(root / "bus"),
            DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(root / "no-system-bus"),
            PATH=":".join([str(root / "bin"), str(Path(controller).parent), *command_directories]),
            UMBRIEL_DINIT_TEST_ROOT=str(root), UMBRIEL_DINIT_TEST_BINARY=str(binary),
            UMBRIEL_DINIT_TEST_DBUS_UPDATE=updater,
            WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1", WLR_LIBINPUT_NO_DEVICES="1",
        )
        environment["UMBRIEL_DINIT_TEST_PATH"] = environment["PATH"]
        # Keep the real login profile path while excluding host-wide profile exports.
        profile_names = set(environment) | {
            "UMBRIEL_LOGIN_REENTRY", "UMBRIEL_DINIT_TEST_PHASE", "UMBRIEL_DINIT_TEST_LONG",
        }
        (root / "home" / ".bash_profile").write_text(
            'for name in $(compgen -e); do\ncase "$name" in\n'
            + "|".join(sorted(profile_names)) + ') ;;\n*) unset "$name" ;;\nesac\ndone\n'
            'printf "profile\\n" >> "$UMBRIEL_DINIT_TEST_ROOT/profiles"\n'
            'export PATH="$UMBRIEL_DINIT_TEST_PATH"\n'
            'export UMBRIEL_LOGIN_PROFILE_MARKER="login profile"\n')
        processes = []
        def launch(command, label, env=None):
            output = (root / (label + ".log")).open("w")
            process = subprocess.Popen(command, env=env or environment, stdout=output, stderr=output,
                                       preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_DFL))
            output.close()
            processes.append(process)
            return process
        def control(*args, check=True):
            result = subprocess.run([controller, "--user", "--quiet", *args], env=environment,
                                    capture_output=True, text=True)
            if check:
                assert result.returncode == 0, result.stderr
            return result
        def start(phase):
            env = dict(environment, UMBRIEL_DINIT_TEST_PHASE=phase)
            if phase != "normal":
                env.update(UMBRIEL_LOGIN_REENTRY="1", UMBRIEL_LOGIN_PROFILE_MARKER="login profile")
            return launch([str(launcher)], "launcher-" + phase, env)
        def ready(phase):
            wait_for(lambda: (root / (phase + "-service.json")).exists(), phase + " service")
            wait_for(lambda: (root / (phase + "-app.json")).exists(), phase + " autostart")
            for role in ("service", "app"):
                record = json.loads((root / (phase + "-" + role + ".json")).read_text())
                assert record["UMBRIEL_DINIT_TEST_VALUE"] == literal
                assert record["PATH"] == "/configured/application/path"
                assert record["DINIT_SOCKET_PATH"] == "/configured/application/socket"
                assert record["UMBRIEL_LOGIN_PROFILE_MARKER"] == "login profile"
                assert record["WAYLAND_DISPLAY"] == "wayland-0"
            assert control("is-started", "umbriel-session", check=False).returncode == 0
            if phase == "normal":
                record = bus_environment(phase, "ready")
                assert record["WAYLAND_DISPLAY"] == "wayland-0"
                assert record["UMBRIEL_SOCKET"] == str(root / "umbriel-wayland-0.sock")
                assert record["UMBRIEL_LOGIN_PROFILE_MARKER"] == "login profile"
                assert "UMBRIEL_DINIT_TEST_VALUE" not in record
        def inspect(phase):
            control("stop", "--ignore-unstarted", "inspect", check=False)
            control("start", "inspect")
            record = json.loads((root / (phase + "-inspect.json")).read_text())
            assert "WAYLAND_DISPLAY" not in record and "UMBRIEL_SOCKET" not in record, record
            record = bus_environment(phase)
            assert not record.get("WAYLAND_DISPLAY") and not record.get("UMBRIEL_SOCKET"), record
        def bus_environment(phase, state="stopped"):
            # The activated probe records its inherited environment and exits without claiming a bus name.
            label = phase.replace("-", "_") + "_" + state
            subprocess.run([bus_sender, "--session", "--type=method_call",
                            "--dest=org.umbriel.SessionProbe." + label, "/", "org.umbriel.SessionProbe.Inspect"],
                           env=environment, check=True, capture_output=True, timeout=10)
            record = root / (phase + "-bus-" + label + ".json")
            wait_for(record.exists, "D-Bus activation environment: " + label)
            return json.loads(record.read_text())
        def ipc(*args):
            return subprocess.run([str(binary), *args], env=dict(environment,
                                  UMBRIEL_SOCKET=str(root / "umbriel-wayland-0.sock")),
                                  capture_output=True, text=True)
        try:
            launch(["dbus-daemon", "--nofork", "--config-file=" + str(bus_config)], "bus")
            wait_for(lambda: (root / "bus").is_socket(), "private bus")
            launch([daemon, "--user", "--quiet", "--services-dir", str(services),
                    "--socket-path", str(root / "control"), "boot"], "manager")
            wait_for(lambda: (root / "control").is_socket(), "private manager")
            control("enable", "--from", "umbriel-session", "component")
            (root / "gate-enabled").touch()
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as gate:
                gate.bind(str(root / "gate"))
                gate.listen(1)
                gate.settimeout(10)
                normal = start("normal")
                connection, _ = gate.accept()
                with connection:
                    assert connection.recv(16) == b"waiting"
                    assert control("is-started", "umbriel", check=False).returncode != 0
                    assert not (root / "normal-service.json").exists()
                    assert not (root / "normal-app.json").exists()
                    assert (root / "umbriel-wayland-0.sock").is_socket()
                    connection.sendall(b"release")
            (root / "gate-enabled").unlink()
            (root / "gate").unlink()
            ready("normal")
            argument_launcher = root / "argument-launcher"
            argument_launcher.write_text(launcher.read_text().replace(str(binary), sys.executable))
            argument_launcher.chmod(0o700)
            arguments = [str(helper), "args", "two words", "quote'and\"double", literal]
            direct = launch([str(argument_launcher), *arguments], "arguments",
                            dict(environment, UMBRIEL_DINIT_TEST_PHASE="args"))
            assert direct.wait(timeout=10) == 0
            assert json.loads((root / "args-args.json").read_text())["arguments"] == arguments[2:]
            duplicate = start("duplicate")
            assert duplicate.wait(timeout=10) != 0
            assert ipc("windows", "--json").returncode == 0
            ipc("msg", "session-quit", "skip-confirmation")
            assert normal.wait(timeout=10) == 0
            wait_for(lambda: (root / "normal-app-stopped").exists(), "child stop signal")
            assert (root / "normal-service-stopped").read_text() == "True", "service stopped after compositor"
            inspect("normal")

            crashed = start("crash")
            ready("crash")
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
                peer.connect(str(root / "umbriel-wayland-0.sock"))
                pid, _, _ = struct.unpack("3i", peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            os.kill(pid, signal.SIGKILL)
            assert crashed.wait(timeout=10) != 0
            wait_for(lambda: (root / "crash-service-stopped").exists(), "dependent service after crash")
            inspect("crash")

            interrupted = start("signal")
            ready("signal")
            interrupted.terminate()
            assert interrupted.wait(timeout=10) == 143
            inspect("signal")

            oversized = launch([str(launcher)], "oversized",
                               dict(environment, UMBRIEL_LOGIN_REENTRY="1", UMBRIEL_DINIT_TEST_LONG="x" * 2048))
            assert oversized.wait(timeout=10) != 0
            assert control("is-started", "umbriel", check=False).returncode != 0

            (root / "fail-manager").touch()
            failed = start("failed-manager")
            assert failed.wait(timeout=10) != 0
            assert not (root / "failed-manager-service.json").exists()
            assert not (root / "failed-manager-app.json").exists()
            inspect("failed-manager")
            (root / "fail-manager").unlink()

            for phase in ("failed-import", "interrupted-start"):
                (root / "gate-enabled").touch()
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as gate:
                    gate.bind(str(root / "gate"))
                    gate.listen(1)
                    gate.settimeout(10)
                    pending = start(phase)
                    connection, _ = gate.accept()
                    with connection:
                        assert connection.recv(16) == b"waiting"
                        assert control("is-started", "umbriel", check=False).returncode != 0
                        if phase == "failed-import":
                            connection.sendall(b"fail")
                            assert pending.wait(timeout=10) != 0
                        else:
                            pending.terminate()
                            assert pending.wait(timeout=10) == 143
                (root / "gate-enabled").unlink()
                (root / "gate").unlink()
                assert not (root / (phase + "-service.json")).exists()
                assert not (root / (phase + "-app.json")).exists()
                inspect(phase)
            assert not (root / "should-not-exist").exists()
            print("dinit readiness, environment, service ordering, arguments, crash, failed import and interruption passed")
        except Exception:
            print(control("catlog", "umbriel", check=False).stdout, file=sys.stderr)
            for log in root.glob("*.log"):
                print("\n" + log.name + ":\n" + log.read_text(), file=sys.stderr)
            raise
        finally:
            # Direct children can survive a compositor crash; these PIDs belong only to this fixture.
            helper_descriptors = []
            if (root / "pids").exists():
                for text in (root / "pids").read_text().splitlines():
                    try:
                        descriptor = os.pidfd_open(int(text))
                        helper_descriptors.append(descriptor)
                        signal.pidfd_send_signal(descriptor, signal.SIGTERM)
                    except ProcessLookupError:
                        continue
            for descriptor in helper_descriptors:
                try:
                    if not select.select([descriptor], [], [], 5)[0]:
                        signal.pidfd_send_signal(descriptor, signal.SIGKILL)
                        assert select.select([descriptor], [], [], 5)[0], "fixture child did not exit"
                except ProcessLookupError:
                    pass
                finally:
                    os.close(descriptor)
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)


if __name__ == "__main__":
    main()
