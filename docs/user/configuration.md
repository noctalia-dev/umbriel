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

| Key                         | Type         | Default                 | Description                                                                                                                                                                                                                             |
| --------------------------- | ------------ | ----------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `autostart`                 | string array | `[]`                    | Shell commands run once after startup. Never re-run on config reload.                                                                                                                                                                   |
| `mod_key`                   | string       | Super (Alt when nested) | Modifier represented by `Mod` in keybinds. Accepts `Super`, `Alt`, `Ctrl`, or `Shift`; aliases `Logo`, `Win`, and `Control` are also accepted. Applies on reload.                                                                       |
| `xwayland`                  | bool         | `true`                  | Spawn `xwayland-satellite` for X11 app support. The binary must be installed. Changing this requires a restart.                                                                                                                         |
| `show_cheatsheet`           | bool         | `true`                  | Show the keybinds cheatsheet overlay on startup. If an included file is still missing, Umbriel waits for it to load before showing the overlay. Press any key or mouse button to dismiss, or toggle at runtime via `cheatsheet-toggle`. |
| `focus_on_activate`         | bool         | `false`                 | Focus and reveal windows that request activation. When false, activation marks the window and its workspace urgent without changing workspaces. Window rules can override this per application.                                         |
| `honor_restored_maximize`   | bool         | `false`                 | Honor maximized state restored by applications while their windows open. Later maximize requests are always honored. Applies to newly opened windows.                                                                                   |

## Environment

```toml
[environment]
GTK_THEME = "Adwaita:dark"
QT_QPA_PLATFORMTHEME = "qt5ct"
```

Extra environment variables exported to Umbriel and all spawned commands.
All values must be strings. Applied once at startup; changing this section
requires a restart.

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
- [Workspace Overview](workspace-overview.md): workspace overview and hot corners.
- [Layout](layout.md): scrolling and dwindle layout behavior.
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

Animation settings live in the top-level `animation` section. `duration_ms` and
`curve` set defaults for every event when present; a nested event can override
either value. The master switch makes every transition instant. Each event also
has its own switch.

```toml
[animation]
enabled = true
duration_ms = 200
curve = "snappy"

[animation.windows_in]
enabled = true
duration_ms = 200
curve = "snappy"
style = "popin"       # "popin", "zoom", "slide", "fade", or "none"
scale = 0.85          # 0.1-1.0, used by "popin"

[animation.windows_out]
enabled = true
duration_ms = 200
curve = "snappy"
style = "fade"        # "fade" or "slide"

[animation.windows_move]
enabled = true
duration_ms = 200
curve = "snappy"

[animation.workspaces]
enabled = true
duration_ms = 250
curve = "snappy"

[animation.scratchpad]
enabled = true
duration_ms = 250
curve = "snappy"
dim = 0.2             # 0.0-1.0
blur = false          # requires appearance.blur.enabled
scale = 0.0           # 0 preserves geometry; 0.1-1.0 sizes and centers on entry
maximize = false      # maximize to edges on entry
fullscreen = false    # fullscreen on entry

[animation.border]
enabled = true
duration_ms = 200
curve = "snappy"

[animation.dim_unfocused]
enabled = true
duration_ms = 200
curve = "snappy"
dim = 0.0             # 0.0-1.0

[animation.fade]
enabled = true
duration_ms = 200
curve = "snappy"
```

| Key               | Type   | Default  | Description                                                                 |
| ----------------- | ------ | -------- | --------------------------------------------------------------------------- |
| `enabled`         | bool   | `true`   | Master switch. When false, every animation transition is instant.           |
| `duration_ms`     | int    | `200`    | Default duration for all events when explicitly set (1-10000 milliseconds). |
| `curve`           | string | `snappy` | Default curve for all events when explicitly set.                           |
| `windows_in.*`    | table  |          | Window open transition.                                                     |
| `windows_out.*`   | table  |          | Window close transition using a scene snapshot.                             |
| `windows_move.*`  | table  |          | Window move and resize transitions.                                         |
| `workspaces.*`    | table  |          | Workspace switch transition.                                                |
| `scratchpad.*`    | table  |          | Scratchpad show/hide transition and backdrop.                               |
| `border.*`        | table  |          | Focus-ring color transition in OkLab color space.                           |
| `dim_unfocused.*` | table  |          | Unfocused-window opacity transition; `dim = 0` disables dimming.             |
| `fade.*`          | table  |          | Generic fade used by layer-shell surfaces.                                  |

An event's `enabled = false` makes only that transition instant. Scratchpad
`dim` and `blur` remain active, without a fade, when animation is disabled.
Scratchpad `scale`, `maximize`, and `fullscreen` apply when a window enters the
scratchpad.

Each curve accepts a built-in name such as `linear`, `ease`, `easeout`,
`snappy`, `bounce`, or `elastic`; a cubic bezier string
`"x1,y1,x2,y2"`; or a spring string `"spring: damping,stiffness"`. Bezier x
coordinates must be between 0 and 1. Spring damping must be between 0.01 and 5,
and stiffness between 1 and 1000.

Custom named curves can be registered once and reused by name:

```toml
[animation.beziers]
myBezier = [0.05, 0.9, 0.1, 1.05]

[animation.springs]
myBounce = { damping = 0.5, stiffness = 200 }
```

Then reference them as `curve = "myBezier"` or `curve = "myBounce"` in any
event section.

## Overview

See [Workspace Overview](workspace-overview.md) for overview and hot corner settings.

## Layout

See [Layout](layout.md) for layout settings and behavior.

## Input

See [Input](input.md) for input device and focus settings.
