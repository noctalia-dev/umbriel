# Building from source

Build Umbriel yourself when no package exists for your distribution or when you want to develop it. Manual installations have no automatic upgrade path, so prefer a [distribution package](installation.md) when one is available.

UmbrielFX builds as part of the source tree. No separate SceneFX package is needed.

## Build and install

Install a C++23 compiler, Meson, Ninja, `just`, `pkg-config`, `wayland-scanner`, and the development packages listed in [`PACKAGING.md`](https://github.com/noctalia-dev/umbriel/blob/main/PACKAGING.md#dependencies). Then clone, build, and install Umbriel:

```sh
git clone https://github.com/noctalia-dev/umbriel.git
cd umbriel
just configure release /usr/local
just release
sudo meson install -C build-release --no-rebuild
```

The release binary is written to `build-release/umbriel`. `just debug` builds the debug binary at `build-debug/umbriel`.

Release builds omit unit tests, harness clients, and harness-only IPC commands by default. Packagers can explicitly disable these with Meson's `-Dtests=disabled -Dharness=disabled` options.

## Library requirements

libxkbcommon 1.10.0 or newer is required at build time and runtime. Its pkg-config name is `xkbcommon`; Umbriel uses its server-side modifier API to preserve keyboard locks and latches.

X11 window shapes require the `xcb-shape` library from libxcb. Debian and Ubuntu need `libxcb-shape0-dev` for building and `libxcb-shape0` at runtime. On openSUSE, `libxcb-devel` provides the build files and `libxcb-shape0` provides the runtime library. Arch, Fedora, Alpine, Nix, Guix, and Void bundle the shape library with libxcb.

`jemalloc` is optional and recommended on glibc. Meson's default `-Djemalloc=auto` uses it when the development package is installed. Use `-Djemalloc=enabled` or `-Djemalloc=disabled` to force the choice. The `auto` setting skips it on non-glibc systems.

## Installing under your home directory

The default installation prefix is `/usr/local`. Meson stores the prefix when the build directory is configured. To install under your home directory, configure that prefix explicitly before building, including when reusing an existing build directory:

```sh
just configure release "$HOME/.local"
just release
just install
```

An existing build directory keeps its stored prefix. For a fresh directory, the automatic configure call in `just release` and `just install` does not forward a `prefix=...` override. Use `just configure` to set or change the path, and check the stored `prefix` with `meson configure build-release` before installing. The system-wide example builds as your user and uses elevated permissions only for installation.

For a user-local installation, ensure `$HOME/.local/bin` is on `PATH` in your login profile. On systemd systems, the installed units under `$HOME/.local/lib/systemd/user` also need to be linked into the user manager's search path before starting a native session:

```sh
systemctl --user link \
  "$HOME/.local/lib/systemd/user/umbriel.service" \
  "$HOME/.local/lib/systemd/user/umbriel-session.target" \
  "$HOME/.local/lib/systemd/user/umbriel-shutdown.target"
systemctl --user daemon-reload
```

A user-local install places the session entry under `$HOME/.local/share/wayland-sessions`. Display-manager search paths vary, so the session may need additional display-manager configuration to appear in its menu. You can start it from a TTY with `start-umbriel` after the setup above.

## Building with Nix

Build the package directly:

```sh
nix build
```

The resulting binary is available at `result/bin/umbriel`. For development, enter the project shell and use the Just recipes:

```sh
nix develop
just debug
```

For unit tests, the headless compositor harness, and contributor checks, see [Development Commands](https://github.com/noctalia-dev/umbriel/blob/main/CONTRIBUTING.md#development-commands).
