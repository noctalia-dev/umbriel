# Packaging Umbriel

Notes for distribution packagers. End-user installation documentation lives
in the [README](README.md) and at [docs.noctalia.dev](https://docs.noctalia.dev/umbriel/).

## Package description

Use this short description for package metadata:

> A Wayland compositor built on wlroots.

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

A source tarball or `git archive` of a tag is complete.

Configure, build, and install with the intended final prefix:

```sh
meson setup build --buildtype=release --prefix=/usr
meson compile -C build
meson install -C build
```

The configured data directory is compiled into Umbriel so it can locate the
packaged default configuration. Do not configure with one prefix and relocate
the installed files to another prefix.

The `tests` feature option defaults to `auto`, which defines unit tests, harness
clients, and umbrielfx checks only for unsanitized debug builds, so the release
build above compiles nothing but the compositor and its installed data. Pass
`-Dtests=disabled` to state that intent explicitly, or `-Dtests=enabled` to get
the suite in a release build:

```sh
meson setup build --buildtype=release --prefix=/usr -Dtests=enabled
meson compile -C build
meson test -C build
```

`jemalloc` is optional and recommended on glibc. The `jemalloc` Meson feature
defaults to `auto`. It is skipped on non-glibc systems.

## umbrielfx

Umbriel's scene graph and GLES2 renderer live in `umbrielfx/`, a hard fork of
[SceneFX](https://github.com/wlrfx/scenefx). It builds in-tree as a static
archive and is not installed, so there is no `scenefx` package to satisfy.

An installed SceneFX package is not a substitute and is never consulted. If a
distribution already ships SceneFX, the two are unrelated and can coexist.

The archive is linked as a whole so its internal utility symbols survive
distribution-provided LTO and archive member pruning.

## Dependencies

### Build and link dependencies

- wlroots 0.20.1 or newer, and strictly below 0.21: `umbrielfx` compiles against wlroots' private struct layouts
- wayland-server 1.24 or newer, plus the Wayland client library
- wayland-protocols 1.47 or newer
- xkbcommon
- libinput 1.23 or newer
- libudev, required for native `[drm]` GPU exclusion support
- pixman 0.43 or newer
- libdrm 2.4.129 or newer
- Cairo and PangoCairo
- tomlplusplus
- nlohmann-json
- EGL, GLES2, and GBM
- lcms2, optional; without it `umbrielfx` rejects client ICC profiles and keeps only its parametric color transforms
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

For a native launch with a working systemd user manager, `start-umbriel` runs
the compositor as `umbriel.service`. Umbriel uses `systemd-run` from that
existing systemd installation to place application commands in transient
scopes under `app.slice`. Managed sessions require systemd 254 or newer so
command arguments can be passed without systemd-side environment expansion.
Without a reachable systemd user manager, and for nested launches, Umbriel does
not invoke `systemd-run` and uses the direct launch path.

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
Packages using nonstandard paths must preserve both configured references.

## Configuration lookup

Without `-c`, Umbriel selects the first existing configuration in this order:

1. `$XDG_CONFIG_HOME/umbriel/config.toml`, normally
   `~/.config/umbriel/config.toml`
2. `umbriel/config.toml` under each directory in `$XDG_CONFIG_DIRS`, normally
   `/etc/xdg/umbriel/config.toml`
3. `<datadir>/umbriel/config.toml`, compiled from the Meson installation paths
4. Internal defaults when none of those files exist

Umbriel captures these candidate paths at startup and watches them for the
session. Creating a higher-priority file or removing the current file
re-evaluates the lookup without a restart. A selected file with syntax or
validation errors does not fall through to the next candidate; the last valid
configuration remains active until the file is corrected or removed.

Umbriel never writes a user configuration automatically. An explicit `-c` path
stays fixed and never uses the fallback chain.

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

For native launches with a supported account shell listed in `/etc/shells`,
`start-umbriel` first enters that shell as a noninteractive login shell. This
loads exported values from shell-specific login files such as zsh's
`~/.zprofile`, while interactive files such as `~/.zshrc` remain outside the
session startup path. The direct fallback inherits that environment after
discarding graphical variables owned by the new session.

An external service manager can invoke `start-umbriel` as its service process.
That fast path does not enter the login shell and instead preserves the
manager-provided environment.

The managed path imports the resulting login environment except for `PATH`,
then starts `umbriel.service`. The service inherits `PATH` and other variables
generated from `environment.d`, so the user manager's `PATH` takes precedence
over the login profile's value. Once ready, Umbriel publishes its graphical
session variables and validated `[environment]` assignments to the systemd user
manager, then starts `umbriel-session.target`. Login-profile values other than
`PATH` are also copied to traditional D-Bus activation. Arbitrary configured
values are not copied there. The configured values remain in the user manager
for its lifetime. The launcher activates `umbriel-shutdown.target` and removes
the graphical variables after Umbriel exits.

In a managed native session, Umbriel creates a separate transient scope under
`app.slice` for the `-s` startup command and every autostart, event hook, and
`spawn:` command. Each scope is part of `umbriel-session.target` and bound to
the systemd unit that owns the compositor, normally `umbriel.service`. Stopping
either one, or an unexpected compositor exit, cleans up the scope, including
when session-target activation fails.
An application failure or OOM kill does not propagate in the other direction,
so it cannot stop `umbriel.service`. `systemd-run` waits for scope creation
before it replaces itself with the command. If isolation cannot be confirmed,
the command is aborted instead of running inside `umbriel.service`. Direct and
nested sessions launch the same commands as ordinary compositor children.

No display manager or desktop shell is required by Umbriel itself. It can be
paired with [Noctalia](https://github.com/noctalia-dev/noctalia) for panels,
notifications, launching, locking, and other desktop-shell services,
[noctalia-greeter](https://github.com/noctalia-dev/noctalia-greeter) as the display manager (using greetd), and
[xdg-desktop-portal-umbriel](https://github.com/noctalia-dev/xdg-desktop-portal-umbriel) for portal screen capture
and sharing.

## Contact

- Issues: https://github.com/noctalia-dev/umbriel/issues
- Discord: https://discord.noctalia.dev
