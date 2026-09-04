# Umbriel

[Umbriel](https://github.com/noctalia-dev/umbriel) is a Wayland compositor for daily use, with scrolling and
tiling layouts, per-output workspaces, window rules, blur, shadows, and fluid animations.

It runs independently and can be paired with [Noctalia](https://docs.noctalia.dev/noctalia/), which provides a first-class desktop shell
experience for Umbriel. Umbriel is built in C++23 on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) and
`umbrielfx`, its own hard fork of [SceneFX](https://github.com/wlrfx/scenefx), with Xwayland support provided by
[xwayland-satellite](https://github.com/Supreeeme/xwayland-satellite) and portal screen capture and sharing by
[xdg-desktop-portal-umbriel](https://github.com/noctalia-dev/xdg-desktop-portal-umbriel), an
xdg-desktop-portal backend for Umbriel.

> Umbriel is young and actively evolving. It is usable for daily use today, but configuration keys, keybinds, and behavior can
> change between releases, and you may hit rough edges. We would rather change what feels wrong than promise stability
> we cannot back yet, so treat current defaults as opinions, not contracts.

## Features

- Scrolling, dwindle, and master layouts with per-workspace selection, width presets, animated navigation, and mouse-driven
  resizing and tiled reordering
- Independent workspaces per output, with hotplug support and configurable modes, positions, scales, and transforms
- Floating, pinned, and fullscreen windows with configurable placement, focus, sizing, opacity, and visual effects
- [Per-output scratchpads](scratchpad.md) for temporarily hiding windows, with toggle, move, restore, and focus-next actions
- An animated overview, directional focus, configurable keybinds, submaps, and activation policy
- Blur, shadows, rounded corners, double borders, opacity, and animated position, size, and fade transitions
- Keyboard, pointer, touch, touchpad gestures, XKB configuration, and text-input-v3/input-method-v2 input method support
- Layer shell, session locking, clipboard management, screen capture, output control, and gamma control
- X11 application support through xwayland-satellite
- Live-reloaded TOML configuration with diagnostics and includes, plus [local IPC](ipc.md) with an event stream and
  runtime inspection commands

## Configuration

Umbriel searches the user, XDG system, and packaged config paths in priority order and keeps that lookup live. Creating
a higher-priority config applies it without a session restart. Config files can include other TOML files, and later
files override earlier ones. See [Configuration](configuration.md).

## Starting Umbriel

Installed display-manager sessions use `start-umbriel`. When a systemd user
manager is available, the launcher runs Umbriel as a user service so the
session inherits variables from `environment.d`. On other init systems it
starts the compositor directly.

Run `umbriel` directly for nested development sessions or explicit unmanaged
startup.

From a TTY, start a normal installed session with:

```sh
start-umbriel
```

## Logs

Umbriel writes its main log to
`$XDG_CACHE_HOME/umbriel/umbriel.log`. If `XDG_CACHE_HOME` is unset, the
fallback path is `~/.cache/umbriel/umbriel.log`. The previous file is retained
as `umbriel.log.1` when the current log reaches 1 MiB.

When standard output or standard error is connected to a TTY, raw writes from
Umbriel and its child processes are redirected to
`$XDG_CACHE_HOME/umbriel/umbriel-stderr.log`, or
`~/.cache/umbriel/umbriel-stderr.log` when `XDG_CACHE_HOME` is unset.

## Contributing

Umbriel shares its conventions with [Noctalia](https://github.com/noctalia-dev/noctalia). Bug reports are always
welcome; feature requests are read against the project's
[scope statement](https://github.com/noctalia-dev/umbriel/blob/main/SCOPE.md). For general help and design
discussion, join the community on [Discord](https://discord.noctalia.dev).
