# Umbriel

Umbriel is a Wayland compositor with scrolling and tiling layouts, independent
workspaces per monitor, and configurable visual effects. It works on its own or
with [Noctalia](https://docs.noctalia.dev/noctalia/), a desktop shell designed
to integrate with Umbriel.

> Umbriel is young and actively evolving. Configuration and behavior may change
> between releases, and you may encounter rough edges.

## Start here

1. [Install Umbriel](installation.md).
2. [Copy and edit the starting configuration](configuration.md#starting-configuration).
3. Configure your [outputs](outputs.md), [input devices](input.md), and
   [keybinds](keybinds.md).
4. Choose a [layout](layout.md) and [workspace model](workspaces.md).
5. Use [window rules](window-rules.md) for application-specific behavior.

Umbriel reloads most configuration changes when you save the file. Errors and
warnings appear on screen, and `umbriel validate` can check a configuration
without a running session.

## Features

- Scrolling, Dwindle, and Master layouts
- Independent workspaces and configuration per output
- Floating, pinned, fullscreen, and [scratchpad](scratchpad.md) windows
- Configurable keybinds, gestures, window rules, blur, shadows, and animations
- X11 application support through xwayland-satellite
- Local [IPC](ipc.md) for scripts, panels, and runtime inspection

## Help and contributing

Bug reports are welcome. Feature requests are considered against the project's
[scope statement](https://github.com/noctalia-dev/umbriel/blob/main/SCOPE.md).
For general help and design discussion, join the community on
[Discord](https://discord.noctalia.dev).
