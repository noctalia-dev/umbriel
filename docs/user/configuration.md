# Configuration

Without `-c`, Umbriel checks `$XDG_CONFIG_HOME/umbriel/config.toml` first,
followed by each `$XDG_CONFIG_DIRS/umbriel/config.toml`, then the packaged
`share/umbriel/config.toml`. The candidate paths come from the startup
environment and remain watched for the session. Creating a higher-priority
file or removing the current file re-evaluates the lookup without a restart.
Removal selects the next existing file, or built-in defaults when none remain.

An invalid higher-priority file does not fall through to a lower-priority one.
Umbriel keeps the last working configuration until the file is corrected or
removed. Pass `umbriel -c <path>` to pin one exact path instead of using the
lookup chain. A missing or invalid pinned path never falls back to an implicit
candidate. Umbriel does not create or modify a user config automatically.

## Starting configuration

The packaged starting configuration is
[`examples/config.toml`](../../examples/config.toml). Distribution packages
normally install it under `/usr/share/umbriel/config.toml`. Copy it before
making local changes:

```sh
mkdir -p ~/.config/umbriel
cp /usr/share/umbriel/config.toml ~/.config/umbriel/config.toml
```

With a manual installation using the `/usr/local` prefix, the default example
configuration is available at `/usr/local/share/umbriel/config.toml`. Nix users
should prefer `programs.umbriel.settings` in Home Manager or hjem.

Creating the user file while Umbriel is running applies it automatically, as
do later saves. No logout or restart is needed. If a reload fails, Umbriel
keeps your last working configuration and continues watching the failed
candidate and its included files. Save a corrected file to try the reload
again. Options that require a restart are marked in their reference tables.

## Diagnostics

Warnings and errors from a load or reload appear in a panel at the top of the
primary output, bordered in `colors.warning` or `colors.error` for the highest
severity present. Errors are listed first, since they are the ones that kept the
configuration from applying. Each entry names its file, line, and column.

A panel that reports only warnings hides itself after ten seconds; one that
reports an error stays until the next successful reload. At most six entries are
listed, and the footer counts the rest. `umbriel validate` prints the full list
without a running compositor and exits nonzero when it reports a diagnostic.

## Include

```toml
[include]
files = ["appearance.toml", "keybinds.toml"]
```

Paths are resolved relative to the main config file. A leading `~` or `~/`
expands to your home directory, and `$VAR` or `${VAR}` expands environment
variables. Later files override earlier files, and values in the main file
override every include.

`files` is the only key `[include]` accepts. Anything else in the section is
reported as an unknown key, in the main config and in included files alike.

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
`general.xwayland = true` provides X11 application support through the
`xwayland-satellite` executable. Install `xwayland-satellite` and make sure it
is on `PATH` before starting Umbriel. If it is unavailable, X11 applications
will not work and Umbriel shows a startup warning. Set `xwayland = false` when
X11 support is not needed.

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
| `xwayland`                | bool         | `true`                  | Spawn `xwayland-satellite` for X11 app support. The executable must be installed and on `PATH`; otherwise X11 applications will not work. Changing this requires a restart.                    |
| `show_cheatsheet`         | bool         | `true`                  | Show the keybinds cheatsheet overlay on startup. If an included file is still missing, Umbriel waits for it to load before showing the overlay. Press any key or mouse button to dismiss, or toggle at runtime via `cheatsheet-toggle`. |
| `focus_on_activate`       | bool         | `false`                 | Let unsolicited activation requests add focus and reveal their target. When false, a mapped target is only marked urgent, while an unmapped target still follows its normal `default_focused` map policy. Tokens issued by `spawn:` and client tokens validated from focused input represent user launch intent and may focus the target. Window rules override this per application. |
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

## Events

```toml
[events]
lid_close = "notify-send 'The laptop lid is closed!'"
lid_open = "notify-send 'The laptop lid is open!'"
```

Defines commands that are executed when the laptop lid is closed or opened.

## Idle inhibition

Umbriel supports application idle inhibitors and idle notifications. An
application inhibits screen blanking, locking, and other idle actions only
while its associated surface is mapped and visible. Switching away from its
workspace, hiding a scratchpad window, disabling its output, or locking the
session stops honoring that inhibitor until the surface becomes visible
again. A visible lock surface may provide its own inhibitor while the session
is locked.
