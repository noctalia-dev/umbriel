# Configuration

Umbriel checks `$XDG_CONFIG_HOME/umbriel/config.toml` first, followed by each
`$XDG_CONFIG_DIRS/umbriel/config.toml`, then the packaged
`share/umbriel/config.toml`. Pass `umbriel -c <path>` to use a different file.
The packaged file is [`examples/config.toml`](../../examples/config.toml) and
can be copied into your user config directory as a starting point. Umbriel
does not create or modify a user config automatically.

## Starting configuration

Distribution packages normally install the starting configuration under
`/usr/share/umbriel/config.toml`. Copy it before making local changes:

```sh
mkdir -p ~/.config/umbriel
cp /usr/share/umbriel/config.toml ~/.config/umbriel/config.toml
```

For an installation using another prefix, replace `/usr/share` with that
installation's data directory, commonly `/usr/local/share`. Nix users should
prefer `programs.umbriel.settings` in Home Manager or hjem.

Changes normally apply as soon as you save. If a reload fails, Umbriel keeps
your last working configuration and continues watching included files. Save a
corrected file to try the reload again. Options that require a restart are
marked in their reference tables.

## Include

```toml
[include]
files = ["appearance.toml", "keybinds.toml"]
```

Paths are resolved relative to the main config file. A leading `~` or `~/`
expands to your home directory, and `$VAR` or `${VAR}` expands environment
variables. Later files override earlier files, and values in the main file
override every include.

You can split your config into multiple files for clarity:

```toml
# ~/.config/umbriel/config.toml
[include]
files = [
  "src/general.toml",
  "src/appearance.toml",
  "src/input.toml",
  "src/keybinds.toml",
  "src/rules.toml",
  "src/workspaces.toml",
  "machines/monolith.toml",
]
```

## General

```toml
[general]
autostart = ["noctalia", "kitty"]
mod_key = "Super"
xwayland = true
show_cheatsheet = true
focus_on_activate = false
honor_restored_maximize = false
```

| Key                       | Type         | Default                 | Description                                                                                                                                                                                                                             |
| ------------------------- | ------------ | ----------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `autostart`               | string array | `[]`                    | Shell commands run once after startup. Never re-run on config reload.                                                                                                                                                                   |
| `mod_key`                 | string       | Super (Alt when nested) | Modifier represented by `Mod` in keybinds. Accepts `Super`, `Alt`, `Ctrl`, or `Shift`; aliases `Logo`, `Win`, and `Control` are also accepted. Applies on reload.                                                                       |
| `xwayland`                | bool         | `true`                  | Spawn `xwayland-satellite` for X11 app support. The binary must be installed. Changing this requires a restart.                                                                                                                         |
| `show_cheatsheet`         | bool         | `true`                  | Show the keybinds cheatsheet overlay on startup. If an included file is still missing, Umbriel waits for it to load before showing the overlay. Press any key or mouse button to dismiss, or toggle at runtime via `cheatsheet-toggle`. |
| `focus_on_activate`       | bool         | `false`                 | Focus and reveal windows that request activation. When false, activation marks the window and its workspace urgent without changing workspaces. Window rules can override this per application.                                         |
| `honor_restored_maximize` | bool         | `false`                 | Honor maximized state requested by applications before their first buffer maps. The first visible configure then uses the final maximized layout target. A request sent after mapping is a normal runtime maximize request and can resize an already visible window. Later maximize requests are always honored. Applies to newly opened windows. |

## Environment

```toml
[environment]
GTK_THEME = "Adwaita:dark"
QT_QPA_PLATFORMTHEME = "qt5ct"
```

Umbriel exports these variables to itself and commands it starts. In a native
session, it also publishes them to the systemd user manager before
`umbriel-session.target` starts. Systemd session services such as Noctalia
inherit the same values, as do applications they launch. D-Bus receives the
graphical connection variables but not arbitrary configured variables, because
they are intended for systemd-managed session services. A nested Umbriel session
does not modify the host session environment. Without a reachable systemd user
manager, the values still apply to Umbriel and commands it starts directly.

Published values remain in the systemd user manager until it exits or another
process changes them. After removing a key from the config, run
`systemctl --user unset-environment NAME` to remove its previous manager value,
or wait until the user manager exits.

Names must match `[A-Za-z_][A-Za-z0-9_]*`, and all values must be strings. This
section cannot override `WAYLAND_DISPLAY`, `WAYLAND_SOCKET`, `DISPLAY`,
`UMBRIEL_SOCKET`, `XDG_CURRENT_DESKTOP`, `XDG_SESSION_DESKTOP`, or
`XDG_SESSION_TYPE`, which Umbriel owns. It is applied only at startup. Config
reload does not update environments already captured by running processes.
Restart Umbriel after changing it, then fully quit and relaunch long-running
applications such as Steam if they survived the session restart.

## Idle inhibition

Umbriel supports application idle inhibitors and idle notifications. An
application inhibits screen blanking, locking, and other idle actions only
while its associated surface is mapped and visible. Switching away from its
workspace, hiding a scratchpad window, disabling its output, or locking the
session stops honoring that inhibitor until the surface becomes visible
again. A visible lock surface may provide its own inhibitor while the session
is locked.

## Configuration topics

Use these pages for detailed topic references:

- [Appearance](appearance.md): colors, borders, blur, and shadows.
- [Animation](animation.md): transition timing, curves, and event-specific effects.
- [Workspace Overview](workspace-overview.md): workspace overview and hot corners.
- [Layout](layout.md): scrolling, dwindle, and master layout behavior.
- [Input](input.md): keyboard, pointer, tablet, cursor, and focus settings.
- [Keybinds](keybinds.md): binding syntax, submaps, and binding behavior.
- [Actions](actions.md): the complete action reference.
- [Outputs](outputs.md): monitor configuration and output movement.
- [Workspaces](workspaces.md): workspace models and workspace rules.
- [Rules](rules.md): window and layer matching.

The detailed sections formerly kept on this page remain reachable through these
short compatibility links:

## Workspaces

See [Workspaces](workspaces.md) for workspace settings and behavior.

## Colors

See [Appearance](appearance.md) for the color reference.

## Appearance

See [Appearance](appearance.md) for appearance, blur, and shadow settings.

## Animation

See [Animation](animation.md) for transition timing, curves, and event-specific
effects.

## Overview

See [Workspace Overview](workspace-overview.md) for overview and hot corner settings.

## Layout

See [Layout](layout.md) for layout settings and behavior.

## Input

See [Input](input.md) for input device and focus settings.
