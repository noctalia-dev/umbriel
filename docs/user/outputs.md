# Outputs

Output sections configure individual monitors. Names must exactly match
connector names such as `DP-1` or `HDMI-A-1`. Nested outputs use `WL-1`;
headless outputs use `HEADLESS-1`.

When an output is disconnected or disabled through configuration, Umbriel moves
its windows to the active workspace on another enabled output. Scratchpad
windows move with that output assignment. If no enabled output remains, windows
stay without a workspace until one becomes available.

Run `umbriel outputs` inside a session to list connector names and modes.

```toml
[output.DP-1]
mode = "3840x2160@165"
position = [0, 0]
scale = 1.25
vrr = "fullscreen"
workspaces = 5
```

## Settings

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enabled` | bool | `true` | Set to `false` to turn the monitor off and remove it from the desktop. |
| `mode` | string | (native) | Resolution and refresh rate: `"WIDTHxHEIGHT"` or `"WIDTHxHEIGHT@HZ"`. Fractional Hz allowed. Ignored in nested sessions (the parent controls size). |
| `position` | `[x, y]` | (auto) | Layout coordinates. |
| `scale` | float | (auto) | Output scale (0.25-4.0). |
| `vrr` | string | `"disabled"` | Variable refresh rate policy: `"disabled"`, `"always"`, or `"fullscreen"`. |
| `hdr` | string | `"off"` | HDR policy: `"off"`, `"on"`, `"auto"`, or `"fullscreen"`. |
| `sdr_white` | float | `203` | SDR reference white in cd/m2 while the output is in HDR mode (80-1000). |
| `workspaces` | int, string array, or `"dynamic"` | `"dynamic"` | Dynamic numbered workspaces, a static count from 1 to 64, or a static ordered list of 1 to 64 names. |
| `transform` | string | `"normal"` | Output rotation/flip. |

### Transform values

`normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180`,
`flipped-270`.

### Variable refresh rate

VRR accepts these policies:

| Value | Behavior |
|-------|----------|
| `"disabled"` | Never enable adaptive sync. This is the default. |
| `"always"` | Keep adaptive sync enabled whenever the output supports it. |
| `"fullscreen"` | Enable adaptive sync only while the active workspace contains a mapped fullscreen window. |

With `"fullscreen"`, switching away from the fullscreen workspace, leaving
fullscreen, or closing the window disables VRR again.

A focused window can override this output policy with the window-rule `vrr`
key. See [window rules](rules.md#settings-updated-while-a-window-is-open).

```toml
[output.DP-1]
mode = "2560x1440@165"
vrr = "fullscreen"
```

Umbriel logs a warning and keeps VRR disabled if the output does not support
adaptive sync or rejects the request. Nested Wayland outputs normally depend on
the parent compositor and may not expose adaptive sync support.

### HDR

HDR accepts these policies:

| Value | Behavior |
|-------|----------|
| `"off"` | Keep the output in its normal SDR mode. This is the default. |
| `"on"` | Keep the output in PQ and BT.2020 continuously. SDR surfaces are mapped to `sdr_white`. |
| `"auto"` | Enable PQ and BT.2020 while a fullscreen surface declaring PQ and BT.2020 is visible on the active workspace. |
| `"fullscreen"` | Enable PQ and BT.2020 while any fullscreen surface is visible on the active workspace. |

Automatic HDR tracks the fullscreen surface that triggered the transition.
Other applications that adopt the HDR output color space after activation do
not keep HDR enabled. Leaving fullscreen, changing workspace, moving the
surface to another output, unmapping it, or closing it returns the output to
SDR.

Automatic HDR cannot infer a color space from pixel values. Direct XWayland
games and other clients that do not attach color-management metadata remain
undetectable. Use a native Wayland HDR path or `hdr = "on"` for those clients.
Automatic activation also requires fullscreen content on the active workspace;
windowed HDR content does not activate the output.

The `"fullscreen"` policy activates HDR before a client supplies color
metadata. This can break the discovery loop for native Wayland games that only
offer HDR after seeing an HDR output. It also activates for fullscreen SDR
applications, including browsers and video players. Untagged surfaces are
still interpreted as SDR and mapped to `sdr_white`; this policy cannot recover
missing HDR color information from a direct XWayland game.

A focused window can override the output policy with the window-rule `hdr`
setting. The same `"off"`, `"on"`, `"auto"`, and `"fullscreen"` values are
accepted. The output policy applies again when focus moves to a window without
an HDR override. This only controls output activation; it does not assign a
color space to an untagged surface.

```toml
[output.DP-1]
hdr = "auto"
sdr_white = 203
```

Switching between SDR and HDR changes the output format, color space, and HDR
metadata. Many monitors briefly go black while their display link resynchronizes.
This is expected for each automatic or fullscreen transition.

While an HDR output is active, screencopy clients such as `grim` and Noctalia
receive an SDR Gamma 2.2 view instead of PQ-encoded output pixels. This keeps
screenshots readable in ordinary SDR viewers. Values outside the SDR capture
range are clipped rather than tone-mapped. Raw export-DMA-BUF capture remains
in the output's native format.

## Disabling an output

Set `enabled = false` on an output section to turn the monitor off. The
connector is powered down, the output leaves the output layout, and its
workspaces no longer appear in the overview. The output's workspaces and their
windows are preserved, so setting `enabled = true` back (or removing the key)
restores the monitor exactly as it was. A disabled output is never picked as a
focus, placement, or layer-surface target.

```toml
[output.HDMI-A-1]
enabled = false
```

Changing `enabled` applies on the next config reload, like the other output
settings. Only the config file can disable an output; see below.

## Display power management

Use `dpms-off` and `dpms-on` to power configured monitors off and on without
removing them from the output layout or moving their workspaces and windows.
The bare actions target every configured output. Add a connector name to target
one monitor:

```sh
umbriel msg dpms-off
umbriel msg dpms-off:DP-1
umbriel msg dpms-on:DP-1
```

Any keyboard, pointer, touch, gesture, or tablet activity powers all DPMS-off
outputs back on. This includes pointer motion. Outputs disabled with
`enabled = false` remain disabled and are not affected by these actions.

## Live reconfiguration

Umbriel implements `wlr-output-management-unstable-v1`, so tools such as
`wlr-randr`, `kanshi`, and `wdisplays` can query and change mode, position,
scale, transform, and adaptive sync at runtime without editing the config file.
`umbriel outputs` only reads from this protocol; it does not send configuration
requests itself.

Requests that disable an output through this protocol are rejected: the
protocol commit would bypass the layout and overview handling that the config
`enabled` key performs. Use `enabled = false` instead.

## Moving focus and windows between outputs

`output-focus-left/right/up/down` move keyboard focus to the adjacent
monitor in that direction. `window-move-to-output-*` and
`column-move-to-output-*` move the focused window, or its whole column, to
the adjacent monitor's active workspace. `workspace-move-to-output-*` instead
creates a new workspace on the adjacent monitor and moves every window of the
active workspace into it, preserving column order and widths. See
[keybinds](keybinds.md) for the full list and their exact semantics.

Direction is determined from output centers in logical layout coordinates.
Small overlaps caused by fractional scaling and coordinate rounding therefore
do not prevent vertically or horizontally arranged outputs from being found.

## Multi-monitor example

A triple-monitor setup with a 4K primary, a 1440p top monitor, and a 1080p
side panel:

```toml
[output.DP-1]
mode = "3840x2160@165"
position = [0, 0]
scale = 1.25
workspaces = 5

[output.DP-2]
mode = "2560x1440@144"
position = [1300, -1440]
scale = 1.0
workspaces = ["VIDEO"]

[output.HDMI-A-1]
mode = "1920x1080@60"
position = [3072, 0]
scale = 1.0
workspaces = ["CHAT", "STATS"]
```

Tiled windows are clipped to the logical bounds of their owning output.
Partially visible scrolling columns do not render onto adjacent outputs,
including when either output uses fractional scaling.

## Machine-specific overrides

A common pattern is to keep output configuration in a separate per-machine
include file so the same base config works on different hardware:

```toml
# ~/.config/umbriel/config.toml
[include]
files = [
  "src/general.toml",
  "src/keybinds.toml",
  "machines/monolith.toml",   # output config for this machine
]
```

## Choose a workspace model

Each output can use dynamic or static workspaces.

### Dynamic workspaces

Omit `workspaces` or set it to `"dynamic"`. The output starts with one empty
workspace named `"1"`. When the last workspace gains a window, Umbriel adds
another empty workspace.

After you leave an empty workspace, Umbriel removes it unless it is still
active. The remaining workspaces are renumbered. If you switch to a workspace
number beyond the current count, Umbriel uses the last workspace.

### Static workspaces

Set `workspaces` to a number or an ordered list of names. Umbriel creates
exactly those workspaces and keeps them when they are empty.

Numeric workspace actions select positions on the focused output, so
`workspace-switch:2` selects the second entry even when that workspace has a
custom name.

```toml
[output.DP-1]
workspaces = 5

[output.DP-2]
workspaces = ["WEB", "CHAT", "VIDEO"]
```

### Change workspaces on reload

Workspace changes apply when you save a valid configuration. For static
workspaces, Umbriel first matches existing workspaces by name and then by
position. Windows from a removed workspace move to the nearest remaining one.

Switching to dynamic workspaces keeps populated and active workspaces,
renumbers them, and adds an empty workspace at the end.

Other output and layout settings are refreshed during a reload as well.

## Workspace rules

`[[workspace]]` entries customize static workspaces or numbered positions on a
dynamic output. They change layout settings but do not create workspaces.

Each rule selects a workspace by exactly one of `name` (string) or `index`
(1-based integer from 1 to 64). An optional `output` restricts the rule to that
output.

### How settings are combined

Workspace layout settings are applied in this order:

1. The base `[layout]` settings.
2. A matching `[[workspace]]` rule without an `output`.
3. A matching `[[workspace]]` rule for the selected output.

Later steps take precedence. On dynamic outputs, rules match workspace names
and numbered positions as those workspaces are created or removed.

### Available fields

| Key | Type | Description |
|-----|------|-------------|
| `name` | string | Select by workspace name (mutually exclusive with `index`). |
| `index` | int | Select by 1-based position from 1 to 64 (mutually exclusive with `name`). |
| `output` | string | Restrict to this output. |
| `layout.mode` | string | `"scrolling"` or `"dwindle"`. |
| `layout.gap` | int | Gap in pixels (0-500). |
| `layout.width_presets` | float array | Widths used by the width-cycle action in both layouts. |
| `layout.scrolling.default_width_fraction` | float | Initial scrolling column width (0.1-1.0). |
| `layout.scrolling.center_underfull_strip` | bool | Center the complete strip whenever it is narrower than the viewport. Disable to left-align underfull strips. |
| `layout.scrolling.direction` | string | `"horizontal"` or `"vertical"` scroll axis. |

### Examples

```toml
# Dwindle layout for the VIDEO workspace on DP-2
[[workspace]]
output = "DP-2"
name = "VIDEO"
layout.mode = "dwindle"

# Scrolling for CHAT, dwindle for STATS, both on HDMI-A-1
[[workspace]]
output = "HDMI-A-1"
name = "CHAT"
layout.mode = "scrolling"
layout.scrolling.direction = "vertical"

[[workspace]]
output = "HDMI-A-1"
name = "STATS"
layout.mode = "dwindle"

# Customize workspace position 4 on DP-1
[[workspace]]
index = 4
output = "DP-1"
layout.gap = 0
layout.scrolling.default_width_fraction = 0.667
```
