# Configuration

Umbriel checks `$XDG_CONFIG_HOME/umbriel/config.toml` first, followed by each
`$XDG_CONFIG_DIRS/umbriel/config.toml`, then the packaged
`share/umbriel/config.toml`. Pass `umbriel -c <path>` to use a different file.
The packaged file is [`examples/config.toml`](../../examples/config.toml) and
can be copied into your user config directory as a starting point. Umbriel
does not create or modify a user config automatically.
Initial configuration errors retain the compatibility fallback to built-in
defaults. When a nonempty `[drm]` policy is present, initial errors are fatal
because falling back would silently allow GPUs that the policy meant to exclude.
Warnings do not prevent the session from starting.

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
| `focus_on_activate`       | bool         | `false`                 | Let unsolicited activation requests add focus and reveal their target. When false, a mapped target is only marked urgent, while an unmapped target still follows its normal `default_focused` map policy. Tokens issued by `spawn:` and client tokens validated from focused input represent user launch intent and may focus the target. Window rules override this per application. |
| `honor_restored_maximize` | bool         | `false`                 | Honor maximized state requested by applications before their first buffer maps. The first visible configure then uses the final maximized layout target. A request sent after mapping is a normal runtime maximize request and can resize an already visible window. Later maximize requests are always honored. Applies to newly opened windows. |

## DRM devices

The optional `[drm]` section selects the renderer GPU and excludes GPUs from a
native session. It is inactive for nested Wayland, X11, and headless backends.
All keys are optional and require a restart.

```toml
[drm]
render_device = "/dev/dri/by-path/pci-0000:05:00.0-render"
ignored_devices = ["/dev/dri/by-path/pci-0000:01:00.0-card"]
# Add the PCI address when the GPU may have no DRM node, such as with VFIO.
ignored_pci_addresses = ["0000:01:00.0"]
```

An incorrect exclusion can prevent startup. Losing the primary GPU ends the
session. Keep another TTY or session available while testing changes.

| Key                     | Type         | Default | Description |
| ----------------------- | ------------ | ------- | ----------- |
| `render_device`         | string       | unset   | Absolute render-node path for the renderer. If Umbriel cannot use it, Umbriel warns and falls back to the primary allowed GPU. |
| `ignored_devices`       | string array | `[]`    | Absolute DRM card or render-node paths. Either node excludes the whole physical GPU. A missing path remains active and resolves again on GPU add or session resume. |
| `ignored_pci_addresses` | string array | `[]`    | PCI addresses in full `domain:bus:slot.function` form. This option can identify a GPU without a DRM node. Umbriel stores addresses in lowercase. |

`umbriel validate` checks path and PCI syntax but does not inspect hardware.
Device type, permissions, seat membership, and KMS support are checked when a
native session starts. An existing path that is not a DRM node is a startup
error.

Prefer stable `/dev/dri/by-path` links over numbered `cardN` and `renderDN`
paths because numbered paths can change across boots. Use a PCI address if the
GPU may start unbound or attached to VFIO. A path alone cannot identify a GPU
if the path has never existed and udev does not advertise it.

Umbriel excludes GPUs before opening their KMS nodes and prefers the allowed
boot-display GPU as primary. Startup fails if no allowed GPU initializes or if
a graphics driver opens an excluded GPU.

### Limits

The DRM policy has these limits:

- A renderer GPU that differs from the primary GPU requires compatible DMA-BUF
  formats and modifiers. It can also increase copying and memory bandwidth.
- Secondary GPUs import DMA-BUFs directly. An incompatible GPU cannot drive an
  output.
- Losing the primary GPU ends the session. Umbriel does not change the primary
  while allocators and outputs use it.
- `WLR_RENDERER_FORCE_SOFTWARE=1` is incompatible with exclusions.

### Environment compatibility

`render_device` overrides `WLR_RENDER_DRM_DEVICE`. Exclusions override
`WLR_DRM_DEVICES`, but a configuration with only `render_device` preserves
`WLR_DRM_DEVICES`.

With exclusions, `WLR_BACKENDS` may contain one `drm` entry and one optional
`libinput` entry. Other combinations that include `drm`, such as
`drm,headless`, fail at startup.

See [DRM device policy](../design/drm-device-policy.md) for the backend and
renderer design.

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
