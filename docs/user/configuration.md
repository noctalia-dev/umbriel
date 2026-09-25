# Configuration

## Starting configuration

The packaged starting configuration is
[`examples/config.toml`](https://github.com/noctalia-dev/umbriel/blob/main/examples/config.toml).
Copy it before making local changes:

```sh
mkdir -p ~/.config/umbriel
cp /usr/share/umbriel/config.toml ~/.config/umbriel/config.toml
```

A manual installation under `/usr/local` places it at
`/usr/local/share/umbriel/config.toml`. Nix users should prefer
`programs.umbriel.settings` in Home Manager or hjem.

Umbriel watches the active configuration and applies valid changes when you
save. Most options reload immediately; reference tables identify options that
require a restart. If a reload fails, Umbriel keeps the last working
configuration and tries again on the next save.

Without `-c`, Umbriel checks these locations in order:

1. `$XDG_CONFIG_HOME/umbriel/config.toml`
2. Each `$XDG_CONFIG_DIRS/umbriel/config.toml`
3. The packaged `share/umbriel/config.toml`

Use `umbriel -c <path>` to select one exact file. Umbriel never creates or
modifies a user configuration automatically.

## Diagnostics

Configuration warnings and errors appear in a panel on the primary output.
Errors keep the previous working configuration active. A warning-only panel
closes after ten seconds; an error remains until the next successful reload.

Check a file without starting the compositor:

```sh
umbriel validate
```

The command prints every diagnostic with its file, line, and column, and exits
nonzero when it finds a problem.

## Include

Split a configuration into smaller files with required or optional includes:

```toml
[include]
files = [
  "appearance.toml",
  "keybinds.toml",
]

[include.optional]
files = [
  "~/.config/umbriel/noctalia.toml",
]
```

Paths are relative to the file containing the include. `~`, `$VAR`, and
`${VAR}` are expanded. Missing optional files are ignored and watched, so
creating one later reloads the configuration.

Included files are applied in list order. The including file is applied last:

- Tables merge by key.
- Rule lists such as `[[window_rule]]` and `[[workspace]]` collect entries.
- Plain arrays and scalar values are replaced by the last file that sets them.
- Setting a rule list to `[]` discards entries collected earlier.

Every file must contain valid TOML. Duplicate device or workspace selectors are
still errors when they come from different files.

If any included file defines `[drm]`, also declare `[drm]` in the main file.
This prevents an incomplete GPU exclusion policy from loading when an include
is unavailable.

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

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `autostart` | string array | `[]` | Commands started once with the session. Changes require a restart. |
| `mod_key` | string | Super, or Alt when nested | Modifier represented by `Mod` in keybinds. |
| `xwayland` | bool | `true` | Start xwayland-satellite for X11 applications. Changes require a restart. |
| `show_cheatsheet` | bool | `true` | Show the keybind cheatsheet when Umbriel starts. |
| `focus_on_activate` | bool | `false` | Let application activation requests focus and reveal their target. |
| `honor_restored_maximize` | bool | `false` | Honor maximize requests an application makes while its window opens, until it acknowledges its opening layout. |

`xwayland-satellite` must be installed and available on `PATH` when X11 support
is enabled.

## DRM devices

Use `[drm]` only when Umbriel should leave a GPU unopened in a native session.
Omit the section for automatic GPU discovery. Changes require a restart.

```toml
[drm]
ignored_pci_addresses = ["0000:01:00.0"]
# ignored_devices = ["/dev/dri/by-path/pci-0000:01:00.0-card"]
```

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `ignored_devices` | string array | `[]` | Absolute card or render-node paths. Prefer stable `/dev/dri/by-path` links. |
| `ignored_pci_addresses` | string array | `[]` | PCI addresses in `domain:bus:slot.function` form. |

Umbriel does not bind or unbind PCI drivers. Configure that lifecycle in
libvirt or equivalent host tooling.

### Limits

- GPU exclusions affect native sessions only.
- Startup fails when no allowed GPU can initialize.
- Secondary GPUs must support the primary GPU's DMA-BUF formats and modifiers.
- Software rendering is incompatible with GPU exclusions.
- With exclusions, `WLR_BACKENDS` supports only `drm` and optional `libinput`.

## Environment

```toml
[environment]
ELECTRON_OZONE_PLATFORM_HINT = "auto"
SDL_VIDEODRIVER = "wayland"
```

These variables apply to Umbriel and commands started in its session. In a
managed native session, they are also published to the systemd user manager for
session services such as Noctalia. Nested sessions do not modify the host
session environment.

Names must match `[A-Za-z_][A-Za-z0-9_]*`, and values must be strings. Umbriel
owns its display and session variables, so this section cannot override
`WAYLAND_DISPLAY`, `WAYLAND_SOCKET`, `DISPLAY`, `UMBRIEL_SOCKET`,
`XDG_CURRENT_DESKTOP`, `XDG_SESSION_DESKTOP`, or `XDG_SESSION_TYPE`.

Environment changes require an Umbriel restart. Fully quit and relaunch
long-running applications that survived the restart.

Removing a key does not clear a value already published to the systemd user
manager. Run `systemctl --user unset-environment NAME` to remove it immediately,
or wait for the user manager to exit.

## Events

Run commands when the laptop lid closes or opens:

```toml
[events]
lid_close = "notify-send 'The laptop lid is closed!'"
lid_open = "notify-send 'The laptop lid is open!'"
```

## Scratchpads

With no `[[scratchpad]]` entries, Umbriel provides one implicit scratchpad named
`default`:

```toml
[keybinds]
"Mod+Shift+Space" = "window-move-to-scratchpad"
"Mod+Space" = "scratchpad-toggle"
```

To configure several scratchpads, give each one a unique name and include that
name in its actions:

```toml
[[scratchpad]]
name = "terminal"

[[scratchpad]]
name = "music"

[keybinds]
"Mod+Space" = "scratchpad-toggle:terminal"
"Mod+M" = "scratchpad-toggle:music"
```

Defining a named scratchpad removes the implicit `default`. See
[Scratchpads](scratchpad.md) for window assignment and behavior.

## Idle inhibition

Umbriel honors idle inhibitors only while their application surface is mapped
and visible. Switching away from its workspace, hiding it in a scratchpad,
disabling its output, or locking the session suspends the inhibitor until the
surface becomes visible again.
