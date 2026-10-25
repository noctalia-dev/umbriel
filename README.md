# Umbriel

Umbriel is a Wayland compositor designed for daily use, with scrolling, dwindle, master, and floating layouts, per-output workspaces, window rules, blur, shadows, and fluid animations.

It runs independently and pairs with [Noctalia](https://github.com/noctalia-dev/noctalia), which provides a first-class desktop shell on top. Under the hood: C++23, [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots), and `umbrielfx`, an in-tree hard fork of [SceneFX](https://github.com/wlrfx/scenefx).

> [!IMPORTANT]
> The project is young and actively evolving. It is usable today, but configuration keys, keybinds, and behavior may change between releases, and rough edges remain. Current defaults are opinions, not stability promises.

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

There is no single killer feature: we were disappointed with the choices available, so we built the compositor we wanted to live in. That is how Noctalia came to life, and this is its compositor side.

We want one desktop for productivity and gaming: flexible layouts, tab groups, and scratchpads for organizing work, alongside HDR, variable refresh rate, and configurable fullscreen presentation for games.

To understand the values and philosophy guiding the project, read our [ethos](https://noctalia.dev/ethos).

## Features

- **Layouts:** scrolling, dwindle, master, and [floating](docs/user/layout.md#floating-layout) per workspace, with [tab groups](docs/user/layout.md#tab-groups), [scratchpads](docs/user/scratchpad.md), and mouse-driven resizing and reordering
- **Workspaces and outputs:** independent workspaces per output, hotplug restoration, fractional scaling, and [virtual outputs](docs/user/outputs.md#virtual-outputs) for [game streaming](docs/user/streaming.md)
- **Windows:** [window rules](docs/user/window-rules.md), [layer rules](docs/user/layer-rules.md), optional [title bars](docs/user/appearance.md#title-bar), [edge snapping](docs/user/input.md#settings), and an animated overview
- **Visuals:** blur, shadows, rounded corners, double borders, animations, and [GLSL effect presets](docs/user/effects.md)
- **Gaming and display:** opt-in [HDR](docs/user/outputs.md#hdr), [variable refresh rate](docs/user/outputs.md#variable-refresh-rate), [tearing](docs/user/outputs.md#tearing), [direct scanout](docs/user/outputs.md#direct-scanout), and explicit sync, where hardware and clients support them
- **Input:** keyboard, touchpad gestures, touch, tablets, input methods, and pointer locking and confinement
- **Integration:** on-demand Xwayland, [sandboxed connections](docs/user/security.md), layer shell, session lock, and screen sharing through [xdg-desktop-portal-umbriel](https://github.com/noctalia-dev/xdg-desktop-portal-umbriel)
- **Configuration:** live-reloaded TOML with includes and diagnostics, plus IPC and runtime inspection

## Install and run

See the [installation guide](docs/user/installation.md) for distribution packages, Nix, and source builds, then pick Umbriel in your display manager or run `start-umbriel` from a TTY. The [default keybinds](docs/user/keybinds.md#default-keybinds) open kitty with mod+Return and the Noctalia launcher with mod alone.

## Configuration

Copy [`examples/config.toml`](examples/config.toml) to `~/.config/umbriel/config.toml` to start; changes apply live. The [online documentation](https://docs.noctalia.dev/umbriel/) is the full reference.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development builds, code style, and checks, and [SCOPE.md](SCOPE.md) for what the project takes on and declines; please skim it before opening a feature request. Bug reports are always welcome, and help and design discussion happen on [Discord](https://discord.noctalia.dev).

## License

MIT License. See [LICENSE](LICENSE) for details.

## Star History

<p align="center">
  <a href="https://github.com/noctalia-dev/noctalia/stargazers">
    <img src="https://api.noctalia.dev/stars/umbriel" alt="Star History" />
  </a>
</p>
