# Packaging Umbriel

Notes for distribution packagers. End-user installation documentation lives
in the [README](README.md) and at [docs.noctalia.dev](https://docs.noctalia.dev/umbriel/).

## Package description

Use this short description for package metadata:

> A Wayland compositor built on wlroots and SceneFX.

## Identity

|                 |                                                                   |
| --------------- | ----------------------------------------------------------------- |
| Name            | `umbriel`                                                         |
| Homepage        | https://github.com/noctalia-dev/umbriel                           |
| Documentation   | https://docs.noctalia.dev/umbriel/                                |
| License         | MIT ([LICENSE](LICENSE))                                          |
| Version         | Meson `project(... version: ...)` in [`meson.build`](meson.build) |
| Binary          | `umbriel`                                                         |
| Session launcher | `start-umbriel`                                                   |
| Wayland session | `umbriel.desktop`                                                 |

Umbriel is Linux-only. The project flake builds `x86_64-linux` and
`aarch64-linux` packages.

## Build

Umbriel requires a C++23 compiler and standard library. It uses Meson and
Ninja, with `pkg-config` and `wayland-scanner` needed during configuration.

Initialize the SceneFX submodule when building from a source checkout or
release archive that represents submodules separately:

```sh
git submodule update --init
```

Configure, build, test, and install with the intended final prefix:

```sh
meson setup build --buildtype=release --prefix=/usr
meson compile -C build
meson test -C build
meson install -C build --skip-subprojects
```

The configured data directory is compiled into Umbriel so it can locate the
packaged default configuration. Do not configure with one prefix and relocate
the installed files to another prefix.

`jemalloc` is optional and recommended on glibc. The `jemalloc` Meson feature
defaults to `auto`. It is skipped on non-glibc systems.

## SceneFX

Umbriel requires APIs from the `umbriel` branch of the
[Noctalia SceneFX fork](https://github.com/noctalia-dev/scenefx/tree/umbriel).
The repository tracks it in `subprojects/scenefx`.

Meson accepts an installed `scenefx-0.5` only when its headers contain the
required patched API. Otherwise it builds the submodule. An unpatched upstream
SceneFX package is not a compatible substitute.

Use `meson install --skip-subprojects` for distribution packages unless the
package intentionally owns the SceneFX installation too.

## Dependencies

### Build and link dependencies

- wlroots 0.20
- Wayland server and client libraries
- wayland-protocols 1.47 or newer
- xkbcommon
- libinput 1.23 or newer
- pixman
- libdrm
- Cairo and PangoCairo
- tomlplusplus
- nlohmann-json
- EGL, GLES, GBM, and related graphics dependencies required by wlroots and SceneFX
- The patched SceneFX fork described above
- jemalloc on glibc, optional

The canonical dependency declarations are in [`meson.build`](meson.build).
Distribution package names vary.

### Runtime dependencies

| Dependency                                 | Role                                                       |
| ------------------------------------------ | ---------------------------------------------------------- |
| `xwayland-satellite`                       | X11 application support when `general.xwayland` is enabled |
| `xdg-desktop-portal-umbriel`               | Screencast and Screenshot portal interfaces for portal-based screen capture |
| A usable font stack                        | Internal overlays and configuration diagnostics            |
| A Wayland-capable graphics and input stack | DRM or nested compositor operation through wlroots         |

`xwayland-satellite` must be discoverable on `PATH`. It may be omitted when a
package or installation deliberately disables Xwayland in the configuration.

On systems with a working systemd user manager, `start-umbriel` runs the
compositor as `umbriel.service`. Other init systems use the direct fallback.

The packaged config contains a `spawn:kitty` keybind as an editable example.
Kitty is not an Umbriel runtime dependency and does not need to be forced into
the compositor package.

## Installed layout

```text
<prefix>/bin/umbriel
<prefix>/bin/start-umbriel
<prefix>/share/umbriel/config.toml
<prefix>/share/wayland-sessions/umbriel.desktop
<prefix>/lib/systemd/user/umbriel.service
<prefix>/lib/systemd/user/umbriel-session.target
<prefix>/lib/systemd/user/umbriel-shutdown.target
```

`share/umbriel/config.toml` is required. It is installed directly from
[`examples/config.toml`](examples/config.toml) and serves as the default when
no user or system configuration exists.

The desktop entry must launch `start-umbriel`. The generated launcher and
`umbriel.service` contain the configured absolute path to the `umbriel` binary.
Packages using nonstandard paths must preserve all three references.

## Configuration lookup

Without `-c`, Umbriel selects the first existing configuration in this order:

1. `$XDG_CONFIG_HOME/umbriel/config.toml`, normally
   `~/.config/umbriel/config.toml`
2. `umbriel/config.toml` under each directory in `$XDG_CONFIG_DIRS`, normally
   `/etc/xdg/umbriel/config.toml`
3. `<datadir>/umbriel/config.toml`, compiled from the Meson installation paths
4. Internal defaults when none of those files exist

Umbriel never writes a user configuration automatically. A selected file with
syntax or validation errors does not fall through to the next candidate.
Explicit `-c` paths never use the fallback chain.

Users can copy the packaged starting point with:

```sh
mkdir -p ~/.config/umbriel
cp /usr/share/umbriel/config.toml ~/.config/umbriel/config.toml
```

Adjust `/usr/share` when using a different installation prefix.

## Nix integration

The Nix package installs the default configuration into its own store output:

```text
/nix/store/<hash>-umbriel/share/umbriel/config.toml
```

That exact data directory is compiled into the corresponding binary. The
NixOS module installs the package and therefore needs no global config file.

Home Manager and hjem leave the user config absent when
`programs.umbriel.settings` is `null`. Providing settings generates
`$XDG_CONFIG_HOME/umbriel/config.toml`, which takes priority over the packaged
file.

## Session integration

Install `umbriel.desktop` under `share/wayland-sessions` so display managers
can discover the session. It invokes `start-umbriel`, which uses the systemd
user manager when available and directly executes Umbriel otherwise.

The managed path imports the display manager environment and starts
`umbriel.service`. The service naturally inherits variables generated from
`environment.d`. Once ready, Umbriel publishes only its graphical-session
variables and starts `umbriel-session.target`. The launcher activates
`umbriel-shutdown.target` and removes those variables after Umbriel exits.

No display manager or desktop shell is required by Umbriel itself. It can be
paired with [Noctalia](https://github.com/noctalia-dev/noctalia) for panels,
notifications, launching, locking, and other desktop-shell services,
[noctalia-greeter](https://github.com/noctalia-dev/noctalia-greeter) as the display manager (using greetd), and
[xdg-desktop-portal-umbriel](https://github.com/noctalia-dev/xdg-desktop-portal-umbriel) for portal screen capture
and sharing.

## Contact

- Issues: https://github.com/noctalia-dev/umbriel/issues
- Discord: https://discord.noctalia.dev
