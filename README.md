# Umbriel

Umbriel is a Wayland compositor designed for daily use, with scrolling, dwindle, and master layouts, per-output
workspaces, window rules, blur, shadows, and fluid animations.

It runs independently and can be paired with [Noctalia](https://github.com/noctalia-dev/noctalia), which provides a
first-class desktop shell experience for Umbriel. Umbriel is built in C++23 on
[wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) and `umbrielfx`, its independently maintained, in-tree hard fork
of [SceneFX](https://github.com/wlrfx/scenefx). Xwayland support comes from wlroots' Xwayland server, which starts on
demand and needs the `Xwayland` binary installed and on `PATH`. Portal screen capture and sharing is provided by
[xdg-desktop-portal-umbriel](https://github.com/noctalia-dev/xdg-desktop-portal-umbriel), an
xdg-desktop-portal backend for Umbriel.

> [!IMPORTANT]
> Umbriel is young and actively evolving. It is usable today, but configuration keys, keybinds, and behavior may change
> between releases, and rough edges remain. Current defaults are opinions, not stability promises.

<p align="center">
  <img src="https://assets.noctalia.dev/?file=umbriel.svg" alt="Umbriel Logo" style="width: 192px" />
</p>

<p align="center">
  <a href="https://docs.noctalia.dev/umbriel/">
    <img src="https://img.shields.io/badge/docs-fbf099?style=for-the-badge&logo=gitbook&logoColor=110f3d&labelColor=fbf099" alt="Documentation" />
  </a>
  <a href="https://discord.noctalia.dev">
    <img src="https://img.shields.io/badge/discord-fbf099?style=for-the-badge&logo=discord&logoColor=110f3d&labelColor=fbf099" alt="Discord" />
  </a>
</p>

## Why Umbriel?

When people ask what Umbriel's selling point is, the honest answer is that there is no single killer feature. We were
simply disappointed with the choices available to us, so we built the compositor we wanted to live in. The plan is
not to conquer the world or take over the big names; it is to feel at home with something we have a say in, with less
friction. That is exactly how Noctalia came to life, and Umbriel is its compositor side.

We want one desktop for productivity and gaming: flexible layouts, tab groups, and scratchpads for organizing work,
alongside HDR, variable refresh rate, and configurable fullscreen presentation for games.

To understand the values and philosophy guiding the project, read our [ethos](https://noctalia.dev/ethos).

## Features

- Scrolling, dwindle, and master layouts with per-workspace selection, width presets, animated navigation, and
  mouse-driven resizing and tiled reordering
- [Tab groups](docs/user/layout.md#tab-groups) that stack, move, and resize like any row in the scrolling layout,
  and tab whole areas in the master layout, with a clickable, scrollable, draggable tab bar that can be hidden
- Independent workspaces per output, with dynamic or named workspaces, hotplug restoration, fractional scaling, and
  configurable modes, positions, and transforms
- Floating, pinned, and fullscreen windows with configurable placement, focus, sizing, opacity, and visual effects
- [Window rules](docs/user/window-rules.md) and [layer rules](docs/user/layer-rules.md) for application placement,
  focus, decorations, and effects
- [Global named scratchpads](docs/user/scratchpad.md) for temporarily hiding
  window groups and summoning them on any output
- An animated overview, directional focus, configurable keybinds, submaps, and activation policy
- Blur, shadows, rounded corners, double borders, opacity, and animated position, size, and fade transitions
- Keyboard, pointer, touch, touchpad gestures, XKB configuration, and text-input-v3/input-method-v2 input method support
- Tablet and pad input, relative pointer motion, pointer locking, and per-window pointer confinement
- [Color-managed HDR](docs/user/outputs.md#hdr) with automatic fullscreen activation, configurable SDR reference white,
  and SDR screen capture, plus optional [10-bit SDR rendering](docs/user/outputs.md#bit-depth)
- [Variable refresh rate](docs/user/outputs.md#variable-refresh-rate), opt-in [tearing](docs/user/outputs.md#tearing),
  and [direct scanout](docs/user/outputs.md#direct-scanout) for eligible fullscreen content
- Explicit GPU synchronization through linux-drm-syncobj when supported by the renderer and backend
- [Restricted Wayland connections](docs/user/security.md) for sandbox engines through security-context-v1, with
  per-application protocol grants
- Layer shell, session locking, clipboard management, screen capture, output control, and gamma control
- On-demand X11 application support through Xwayland, with optional native-resolution rendering on scaled outputs
- [Virtual outputs](docs/user/outputs.md#virtual-outputs) for screen sharing and
  [Sunshine/Moonlight game streaming](docs/user/streaming.md)
- Live-reloaded TOML configuration with diagnostics and includes, plus local IPC and runtime inspection commands
- [GLSL effect presets](docs/user/effects.md) for animations, borders, windows, screens, and cursors, with effect pools,
  runtime selection, and inspection
- Runs as a nested Wayland compositor inside an existing Wayland or X11 desktop for development, or directly on DRM
  for daily use

HDR, VRR, tearing, direct scanout, and explicit synchronization depend on the display, graphics stack, and client.
HDR, VRR, and tearing are opt-in; see the [output reference](docs/user/outputs.md) for policies and fallback behavior.

## Building

Distribution maintainers should also read [PACKAGING.md](PACKAGING.md) for the
installed layout, dependency notes, and config fallback.

The scene graph and renderer live in [`umbrielfx/`](umbrielfx/) and build as part of the tree. They are maintained as
part of Umbriel rather than rebased onto upstream SceneFX; no separate SceneFX package is needed.

### System build

Install a C++23 compiler, Meson, Ninja, Just, pkg-config, wayland-scanner, and development packages for wlroots 0.20
(0.20.1 or newer, built with Xwayland support), Wayland, wayland-protocols, xkbcommon, libinput, pixman, libdrm,
libdisplay-info, EGL, GLES2, GBM, Cairo, Pango, tomlplusplus, nlohmann-json, xcb, xcb-icccm, and xcb-ewmh.
Native `[drm]` GPU exclusions also require libudev. lcms2 and PipeWire (for the `umbriel-audio` helper) are optional. See [PACKAGING.md](PACKAGING.md#dependencies)
for dependency version requirements and test-only dependencies. Then build Umbriel:

```sh
just release
just install
```

`jemalloc` is optional but recommended on glibc: it returns freed memory to the OS promptly and bounds heap
fragmentation in long-running sessions. Meson's `-Djemalloc=enabled` or `-Djemalloc=disabled` forces the choice; the
default (`auto`) uses it when the development package is installed and skips it otherwise (non-glibc libc builds
always skip it).

The binaries are written to `build-debug/umbriel` and `build-release/umbriel`.

### Nix

Build the package directly:

```sh
nix build
```

The resulting binary is available at `result/bin/umbriel`. For development, enter the project shell and use the same
Just recipes as a system build:

```sh
nix develop
just debug
```

For unit tests, the headless compositor harness, and contributor checks, see
[Development Commands](CONTRIBUTING.md#development-commands).

## Running

Installed display-manager sessions start through `start-umbriel`. For supported
account shells, it loads the noninteractive login environment, then runs the
compositor as a user service on systemd or directly on other init systems.
In a managed systemd session, the launcher imports that login environment into
the user manager before starting Umbriel. Login-profile values take precedence
over values with the same names from `environment.d`, including `PATH`;
variables found only in the user manager remain available.

Start an installed native session from a TTY with:

```sh
start-umbriel
```

From an existing Wayland or X11 session, Umbriel opens a nested window (mod = Alt).
From a TTY it takes over the seat (mod = Super).

Apps that capture the screen through xdg-desktop-portal (browser screen sharing, OBS, portal-aware screenshot
tools) are served by [xdg-desktop-portal-umbriel](https://github.com/noctalia-dev/xdg-desktop-portal-umbriel), which
implements the Screencast and Screenshot interfaces for Umbriel.

```sh
just run debug kitty
```

Or run the binary directly:

```sh
./build-debug/umbriel -s kitty
```

With the packaged starting configuration:

| Shortcut | Action |
|----------|--------|
| mod+Escape | Quit (asks for confirmation) |
| mod+F1 | Cycle window focus |
| mod+H/J/K/L or arrows | Focus adjacent window |
| mod+Shift+arrows | Move column left/right or window up/down |
| mod+comma / mod+period | Consume left / consume right |
| mod+R / mod+F | Cycle width / toggle fullscreen |
| mod+T | Toggle floating for the focused window |
| mod+P | Toggle pin for the focused window |
| mod+W | Toggle tabs for the focused column |
| mod+O | Toggle the overview |
| mod+1..9 | Switch workspace on focused monitor |
| mod+Shift+1..9 | Move focused window to workspace and follow |

`kitty` is an optional startup command. Replace it with another command, or omit it by running `just run debug`
or `./build-debug/umbriel`. The packaged configuration binds mod+Return to `spawn:kitty` and mod alone to the Noctalia
launcher. Change those commands under `[keybinds]` if you use another terminal or shell (see
[`examples/config.toml`](examples/config.toml)).

Stop with mod+Escape or `Ctrl+C` from the parent terminal.

## Configuration

Umbriel first checks `$XDG_CONFIG_HOME/umbriel/config.toml`, then
`$XDG_CONFIG_DIRS`, and finally its packaged `share/umbriel/config.toml`.
These paths remain watched, so creating a higher-priority config switches to it
without a session restart. Pass `-c path/to/config.toml` to pin another file.
Config files can include files with
`[include] files = ["theme.toml", "keybinds.toml"]`; later files and the main
file override earlier values.

See [`examples/config.toml`](examples/config.toml) for the packaged starting configuration and
[`our online documentation`](https://docs.noctalia.dev/umbriel/) for the full reference.

### Nix (home-manager / NixOS)

Declarative configuration uses Nix attrsets serialized to TOML with `pkgs.formats.toml`.

```nix
# flake inputs
umbriel.url = "git+https://github.com/noctalia-dev/umbriel";

# NixOS
imports = [ inputs.umbriel.nixosModules.default ];
programs.umbriel.enable = true;

# home-manager
imports = [ inputs.umbriel.homeModules.default ];
programs.umbriel = {
  enable = true;
  settings = {
    general.autostart = [ "noctalia" ];
    layout.gap = 5;
    input.keyboard.layout = "de";
    keybinds = {
      "Mod+Return" = "spawn:kitty";
      "Mod+Q" = "window-close";
      "Mod" = "spawn:noctalia msg panel-toggle launcher";
    };
  };
};
```

The portal lives in [a separate repository](https://github.com/noctalia-dev/xdg-desktop-portal-umbriel) and comes
with the NixOS module: enabling Umbriel installs it, configures it as the `xdg.portal` backend, and writes the
portal configuration screencasting needs. You can set `programs.umbriel.portalPackage` to null if you don't want
the portal.

When `settings` is omitted, the Home Manager and hjem modules leave the user path untouched so Umbriel loads its
packaged configuration. Home Manager also accepts a raw TOML string or a path. The hjem module is exported as
`inputs.umbriel.hjemModules.default`.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for code style, naming conventions, the dependency stack, and debugging
helpers, and [SCOPE.md](SCOPE.md) for what the project takes on and what it declines. Umbriel shares its conventions
with [noctalia](https://github.com/noctalia-dev/noctalia). For general help and design discussion, join the community
on [Discord](https://discord.noctalia.dev).

Bug reports are always welcome. Feature requests are read against [SCOPE.md](SCOPE.md), so please skim it before
opening one, and ask on Discord if you are unsure whether an idea fits.

## License

MIT License. See [LICENSE](LICENSE) for details.

## Star History

<p align="center">
  <a href="https://github.com/noctalia-dev/noctalia/stargazers">
    <img src="https://api.noctalia.dev/stars/umbriel" alt="Star History" />
  </a>
</p>
