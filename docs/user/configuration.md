# Configuration

## Starting configuration

The packaged starting configuration is [`examples/config.toml`](https://github.com/noctalia-dev/umbriel/blob/main/examples/config.toml). Copy it before making local changes:

```sh
mkdir -p ~/.config/umbriel
cp /usr/share/umbriel/config.toml ~/.config/umbriel/config.toml
```

A manual installation under `/usr/local` places it at `/usr/local/share/umbriel/config.toml`. Nix users should prefer `programs.umbriel.settings` in Home Manager or hjem.

Umbriel watches the active configuration and applies valid changes when you save. Most options reload immediately; reference tables identify options that require a restart. If a reload fails, Umbriel keeps the last working configuration and tries again on the next save.

Without `-c`, Umbriel checks these locations in order:

1. `$XDG_CONFIG_HOME/umbriel/config.toml`
2. Each `$XDG_CONFIG_DIRS/umbriel/config.toml`
3. The packaged `share/umbriel/config.toml`

Use `umbriel -c <path>` to select one exact file. Umbriel never creates or modifies a user configuration automatically.

## Diagnostics

Configuration warnings and errors appear in a panel on the primary output. Errors keep the previous working configuration active. A warning-only panel closes after ten seconds; an error remains until the next successful reload.

Check a file without starting the compositor:

```sh
umbriel config validate
```

The command prints every diagnostic with its file, line, and column, and exits nonzero when it finds a problem.

List every configuration key the installed binary accepts:

```sh
umbriel config schema
umbriel config schema --json
```

Without `--json` it prints how many options each section has. With `--json` it prints each key's path, type, range, accepted values, format, and built-in default, plus the binary's `version` and `revision` (`null` for a build outside git). `name[]` marks the entries of an array of tables, such as `window_rule[].match.app_id`, and `<name>` a name you choose, such as `output.<name>.scale`.

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

Paths are relative to the file containing the include. `~`, `$VAR`, and `${VAR}` are expanded. Missing optional files are ignored and watched, so creating one later reloads the configuration.

Included files are applied in list order. The including file is applied last:

- Tables merge by key.
- Rule lists such as `[[window_rule]]` and `[[workspace]]` collect entries.
- Plain arrays and scalar values are replaced by the last file that sets them.
- Setting a rule list to `[]` discards entries collected earlier.

Every file must contain valid TOML. Duplicate device or workspace selectors, and an effect preset defined in two files, are errors even when they come from different files. At startup, any configuration error makes Umbriel use the default configuration and show an error banner; a reload keeps the previous configuration.

If any included file defines `[drm]`, also declare `[drm]` in the main file. This prevents an incomplete GPU exclusion policy from loading when an include is unavailable.

## General

```toml
[general]
autostart = ["noctalia", "kitty"]
mod_key = "Super"
xwayland = true
xwayland_native_resolution = false
xwayland_xsettings = true
xwayland_primary_output = ""
nvidia_sync_workaround = true
show_cheatsheet = true
focus_on_activate = false
attach_modal_dialogs = true
honor_restored_maximize = false
```

| Key                          | Type         | Default                   | Description                                                                                                                                                                          |
| ---------------------------- | ------------ | ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `autostart`                  | string array | `[]`                      | Commands started once with the session. Changes require a restart.                                                                                                                   |
| `mod_key`                    | string       | Super, or Alt when nested | Modifier represented by `Mod` in keybinds.                                                                                                                                           |
| `xwayland`                   | bool         | `true`                    | Start Xwayland lazily for X11 applications. Changes require a restart.                                                                                                               |
| `xwayland_native_resolution` | bool         | `false`                   | Show outputs to X11 applications at their physical resolution, so they render sharp on scaled outputs. Changes require a restart.                                                    |
| `xwayland_xsettings`         | bool         | `true`                    | Announce the interface scale and cursor theme to X11 applications through XSETTINGS. Turn it off to run your own XSETTINGS manager, such as `xsettingsd`. Changes require a restart. |
| `xwayland_primary_output`    | string       | `""`                      | Connector or monitor identity to publish as the X11 primary monitor. Applies on reload. Empty leaves primary selection unmanaged.                                                    |
| `nvidia_sync_workaround`     | bool         | `true`                    | Wait for GPU completion on composed NVIDIA outputs without explicit synchronization. Applies on reload. See [NVIDIA synchronization](#nvidia-synchronization).                       |
| `show_cheatsheet`            | bool         | `true`                    | Show the keybind cheatsheet when Umbriel starts.                                                                                                                                     |
| `focus_on_activate`          | bool         | `false`                   | Let application activation requests focus and reveal their target.                                                                                                                   |
| `attach_modal_dialogs`       | bool         | `true`                    | Attach a modal dialog to its parent, so it follows the parent and holds its input. See [Dialogs](layout.md#dialogs).                                                                 |
| `honor_restored_maximize`    | bool         | `false`                   | Honor maximize requests an application makes while its window opens, until it acknowledges its opening layout.                                                                       |

`Xwayland` must be installed and available on `PATH` when X11 support is enabled. It starts when the first X11 application connects.

X11 bounding shapes clip windows to their visible regions, including holes and separate rectangles. Shape changes apply while the window is open and are preserved in overview previews, window captures, and close animations. Input shapes remain independent, so a click-through overlay can keep drawing its visible widgets.

X11 applications have no per-output scale. By default they see every output at its logical size, and Umbriel upscales their windows on outputs with a scale above 1, which looks soft. With `xwayland_native_resolution = true` they see each output at its rendered size instead — its `render_scale`, or its scale when `render_scale` is unset: a `3840x2160` output at scale `1.25` appears to them as `3840x2160` rather than `3072x1728`. Their windows render at one pixel per rendered pixel and stay sharp, but X11 applications that do not scale their own interface look smaller on scaled outputs. Games benefit most.

With `xwayland_xsettings = true`, Umbriel tells X11 applications that read XSETTINGS, such as GTK, Chromium, Electron, and Steam, the scale to draw their interface at: the largest output render scale with `xwayland_native_resolution = true`, and `1` without it. It also announces `[input.cursor]` `theme` and `size`, the size scaled the same way. At native resolution X11 has one scale for every output, so on outputs with a smaller scale these applications look larger than native ones.

### X11 primary monitor

Some X11 games and Wine applications enumerate resolutions from the X11 primary monitor, independently of where Umbriel places their windows. Select that monitor explicitly:

```toml
[general]
xwayland_primary_output = "DP-1"
```

The setting accepts the same connector names and monitor identities as [output sections](outputs.md). Run `umbriel outputs` to find them. Matching is case-insensitive. Use a monitor identity to follow a display between ports; use a connector to select a physical port. If a name matches several connected monitors with identical identities, Umbriel uses the first matching monitor in its internal layout order; use a connector to distinguish them.

Umbriel applies the choice on reload, when the monitor becomes available, and whenever its lazy Xwayland server restarts. The primary stays stable while focus or the pointer moves between outputs. If the chosen monitor is disconnected or disabled, Umbriel clears the X11 primary until it returns. Removing the setting or setting it to `""` clears Umbriel's previous selection once, then leaves X11 primary selection unmanaged. With the setting omitted from startup, Umbriel preserves externally selected primaries.

Output coordinates can be negative. Xwayland receives the layout translated to nonnegative coordinates, so the monitor at Umbriel's `[0, 0]` need not be at Xwayland's origin. `focus_at_startup` only places the cursor and does not select an X11 primary. Games that cache monitor information may need to be relaunched after changing the primary.

### NVIDIA synchronization

With `nvidia_sync_workaround = true`, Umbriel waits for GPU rendering to finish before presenting a composed output frame when the renderer uses the NVIDIA driver and the output has no explicit synchronization timeline. This can reduce flicker caused by unreliable implicit synchronization. Outputs with explicit synchronization continue to use GPU fences without this extra wait.

Waiting can cause stuttering or frame drops, especially when rendering takes longer than a refresh interval. Set `nvidia_sync_workaround = false` to disable the workaround if it causes those symptoms; synchronization-related flicker may return. Changes apply to subsequent composed frames on reload and survive renderer recovery. Capture synchronization is independent of this setting.

## DRM devices

Use `[drm]` only when Umbriel should leave a GPU unopened in a native session. Omit the section for automatic GPU discovery. Changes require a restart.

```toml
[drm]
ignored_pci_addresses = ["0000:01:00.0"]
# ignored_devices = ["/dev/dri/by-path/pci-0000:01:00.0-card"]
```

| Key                     | Type         | Default | Description                                                                 |
| ----------------------- | ------------ | ------- | --------------------------------------------------------------------------- |
| `ignored_devices`       | string array | `[]`    | Absolute card or render-node paths. Prefer stable `/dev/dri/by-path` links. |
| `ignored_pci_addresses` | string array | `[]`    | PCI addresses in `domain:bus:slot.function` form.                           |

Umbriel does not bind or unbind PCI drivers. Configure that lifecycle in libvirt or equivalent host tooling.

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

These variables apply to Umbriel and commands started in its session. In a managed native session, they are also published to the systemd user manager for session services such as Noctalia. Nested sessions do not modify the host session environment.

Names must match `[A-Za-z_][A-Za-z0-9_]*`, and values must be strings. Umbriel owns its display, input-emulation, and session variables, so this section cannot override `WAYLAND_DISPLAY`, `WAYLAND_SOCKET`, `DISPLAY`, `UMBRIEL_SOCKET`, `LIBEI_SOCKET`, `XDG_CURRENT_DESKTOP`, `XDG_SESSION_DESKTOP`, or `XDG_SESSION_TYPE`.

Environment changes require an Umbriel restart. Fully quit and relaunch long-running applications that survived the restart.

Removing a key does not clear a value already published to the systemd user manager. Run `systemctl --user unset-environment NAME` to remove it immediately, or wait for the user manager to exit.

## Events

Run commands when the laptop lid closes or opens:

```toml
[events]
lid_close = "notify-send 'The laptop lid is closed!'"
lid_open = "notify-send 'The laptop lid is open!'"
```

Umbriel runs the command for the current lid state once during startup, after the compositor IPC socket is ready, then runs commands when that state changes. This also handles signing in while the lid is already closed or already open. If the system reports more than one lid switch, the combined state remains closed while any switch is closed. Device removal and re-addition during suspend do not repeat a command unless the combined state actually changed.

Logical output actions can remove the laptop panel from the desktop instead of merely powering it off:

```toml
[events]
lid_close = "umbriel msg output-disable eDP-1"
lid_open = "umbriel msg output-enable eDP-1"
```

## Screencast

Target-changing screencast actions ask for confirmation the first time they are used during an active single-source share. Dismissing the panel cancels only the pending action, so invoking one again asks again. Approval lasts until the share ends.

```toml
[screencast]
disable_dynamic_confirmation = false
```

| Key                            | Type | Default | Description                                                                                                                                     |
| ------------------------------ | ---- | ------- | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| `disable_dynamic_confirmation` | bool | `false` | Apply set and follow actions immediately without first confirming them. This can expose another window or output after an accidental key press. |

## Scratchpads

With no `[[scratchpad]]` entries, Umbriel provides one implicit scratchpad named `default`:

```toml
[keybinds]
"Mod+Shift+Space" = "window-move-to-scratchpad"
"Mod+Space" = "scratchpad-toggle"
```

To configure several scratchpads, give each one a unique name and include that name in its actions:

```toml
[[scratchpad]]
name = "terminal"

[[scratchpad]]
name = "music"

[keybinds]
"Mod+Space" = "scratchpad-toggle terminal"
"Mod+M" = "scratchpad-toggle music"
```

Defining a named scratchpad removes the implicit `default`. See [Scratchpads](scratchpad.md) for window assignment and behavior.

## Idle inhibition

Umbriel honors idle inhibitors only while their application surface is mapped and visible. Switching away from its workspace, hiding it in a scratchpad, disabling its output, or locking the session suspends the inhibitor until the surface becomes visible again.
