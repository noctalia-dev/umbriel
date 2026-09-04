# Outputs

Output sections configure individual monitors. A section is named either by
connector or by monitor.

A **connector** is `DP-1`, `HDMI-A-1` and so on. Nested outputs use `WL-1`;
headless outputs use `HEADLESS-1`.

A **monitor** name is `"<make> <model> <serial>"`, shown by `umbriel outputs`
as `Config name`, with the literal `Unknown` for any field the display leaves
empty:

```toml
[output."Microstep MSI G2712F CD6T084401192"]
mode = "1920x1080@180"
```

Both forms are matched case-insensitively, and both work anywhere an output is
named: output sections, `default_output` on a window rule, `map_to_output` on a
tablet, `output` on a workspace rule, and the `:OUTPUT` suffix on actions such
as `dpms-off` and `scratchpad-toggle`.

Prefer the monitor form when a rule belongs to a particular display rather than
to a particular port. A connector is a property of the machine, so a laptop used
at two desks sees both monitors as `HDMI-A-1`, and a connector-keyed rule
written for one silently applies to the other, typically as a mode the second
display cannot do. Naming the monitor lets both rules coexist, each applying
only when that display is attached.

When connector and monitor output sections both match, the monitor section
wins. This allows a connector section to provide a port-specific fallback while
a monitor section overrides it for a known display.

A display that reports no make, model or serial can only be named by its
connector. It is not matched as `Unknown Unknown Unknown`, since every such
output would answer to that. Two displays that report the same make, model, and
serial also share a monitor name; use their distinct connectors when both are
connected.

When an output is disconnected or disabled through configuration, Umbriel moves
its windows to the active workspace on another enabled output, and moves them
back to the workspace they came from when it returns. The output also returns
to its previously active workspace. Floating and pinned windows retain their
full-output-relative positions even when a panel recreates its exclusive zone
after the output. Scratchpad windows move with their output assignment and
return with it too. Tiled windows retain their order, grouping, split ratios,
and sizes in the scrolling, dwindle, and master layouts. Taskbars and docks
continue to associate windows on inactive workspaces with the restored output
without requiring each workspace to be visited. If no enabled output remains,
windows stay without a workspace until one becomes available.

Run `umbriel outputs` inside a session to list connector names, copyable monitor
configuration names, and modes. `umbriel outputs --json` prints the same
information as an array. Each entry has the reported `name`, `description`,
`make`, `model`, `serial`, `enabled`, `transform`, and `scale`; logical
`position`; physical `physical_size` in millimetres; nullable `adaptive_sync`;
and `modes`. Every mode has `width`, `height`, `refresh_mhz`, `preferred`, and
`current`. `config_name` is the copyable monitor name or `null` when the display
does not report make, model, or serial.

```toml
[output.DP-1]
mode = "3840x2160@165"
position = [0, 0]
scale = 1.25
vrr = "fullscreen"
tearing = true
direct_scanout = false
workspaces = 5
```

## Settings

| Key                                          | Type                              | Default     | Description                                                                                                                                         |
| -------------------------------------------- | --------------------------------- | ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| `enabled`                                    | bool                              | `true`      | Set to `false` to turn the monitor off and remove it from the desktop.                                                                              |
| `mode`                                       | string                            | (native)    | Resolution and refresh rate: `"WIDTHxHEIGHT"` or `"WIDTHxHEIGHT@HZ"`. Fractional Hz allowed. Ignored in nested sessions (the parent controls size). |
| `position`                                   | `[x, y]`                          | (auto)      | Top-left corner in logical layout coordinates. Omit for automatic placement.                                                                        |
| `scale`                                      | float                             | `1.0`       | Output scale (0.25-4.0).                                                                                                                            |
| `vrr`                                        | string                            | `"disabled"` | Variable refresh rate policy: `"disabled"`, `"always"`, or `"fullscreen"`.                                                                          |
| `tearing`                                    | bool                              | `false`     | Permit asynchronous page flips for eligible fullscreen windows on this output.                                                                      |
| `direct_scanout`                             | bool                              | `true`      | Permit eligible client buffers to bypass composition on this output. Set to `false` to always composite.                                            |
| `hdr`                                        | string                            | `"off"`     | HDR policy: `"off"`, `"on"`, `"auto"`, or `"fullscreen"`.                                                                                           |
| `sdr_white`                                  | float                             | `203`       | SDR reference white in cd/m2 while the output is in HDR mode (80-1000).                                                                             |
| `workspaces`                                 | int, string array, or `"dynamic"` | `"dynamic"` | Dynamic numbered workspaces, a static count from 1 to 64, or a static ordered list of 1 to 64 names.                                                |
| `min_workspaces`                             | int                               | `1`         | Workspace count a dynamic output never shrinks below (1-64). Rejected together with a static `workspaces` inventory.                                |
| `transform`                                  | string                            | `"normal"`  | Output rotation/flip.                                                                                                                               |
| `layout.scrolling.default_width_fraction`    | float                             | inherited   | Initial scrolling strip-axis extent for new columns on this output (0.1-1.0). Inherits the global value when omitted.                               |

### Workspace count

`workspaces` chooses the model: omitted or `"dynamic"` for dynamic numbered
workspaces, a count or ordered name list for a static inventory.

On a dynamic output, `min_workspaces` is a floor on the count. The output keeps
that many workspaces while they are empty, still adds a trailing empty one above
the floor, and prunes back down to it:

```toml
[output.DP-1]
min_workspaces = 3
```

The floor is per output. Setting it alongside a static `workspaces` inventory is
a configuration error, since that inventory already states an exact count. See
[Workspaces](workspaces.md#choose-a-workspace-model).

### Initial scrolling width

Override the global initial scrolling width for every workspace on one output:

```toml
[output.DP-1.layout.scrolling]
default_width_fraction = 0.4
```

The output name uses the same connector or monitor identity matching as the
rest of its section. If connector and monitor sections both match, the monitor
section wins, including for this value.

A matching workspace rule can override the output value. Resolution proceeds
from the global value, to the matching output value, to an unscoped workspace
rule, and finally to an output-scoped workspace rule. See
[Workspace rules](workspaces.md#workspace-rules).

This setting controls initial width only. Reloading it leaves existing columns
at their stored widths, while columns created afterward use the new value.
Moving an existing column onto this output also preserves that column's width.
See [Scrolling behavior](layout.md#scrolling-behavior) for window-rule and
column-creation details.

### Position and scale

An output's logical size is its transformed mode size divided by `scale`. A
`2560x1600` output at scale `1.25` occupies `2048x1280` logical units. If it
starts at `[0, 0]`, an output immediately to its right starts at `[2048, 0]`.
A 1920-wide output at scale `1.0` immediately to its left starts at
`[-1920, 0]`.

The pointer can cross only where output rectangles touch or overlap. Omit
`position` to place outputs automatically from left to right and keep them
adjacent when their mode, scale, or transform changes. Removing a configured
`scale` restores `1.0` on reload.

### Transform values

`normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180`,
`flipped-270`.

### Direct scanout

Direct scanout lets an eligible fullscreen client buffer be presented without
first rendering it into Umbriel's composited output buffer. Disable it for an
output when a fullscreen application causes graphical corruption, black
frames, flicker, or driver-specific presentation problems:

```toml
[output.DP-1]
direct_scanout = false
```

The setting applies on reload. Changing it fully damages and schedules that
output. Setting it to `false` returns an active direct scanout to composition on
the next frame; setting it to `true` makes eligible future frames candidates
for direct scanout. Disabling it can increase GPU use and power consumption
while fullscreen content is visible.

`WLR_SCENE_DISABLE_DIRECT_SCANOUT=1` is a process-wide startup override. When
set, it disables direct scanout on every output regardless of
`direct_scanout = true`.

### Variable refresh rate

VRR accepts these policies:

| Value          | Behavior                                                                                  |
| -------------- | ----------------------------------------------------------------------------------------- |
| `"disabled"`   | Never enable adaptive sync. This is the default.                                          |
| `"always"`     | Keep adaptive sync enabled whenever the output supports it.                               |
| `"fullscreen"` | Enable adaptive sync only while the active workspace contains a mapped fullscreen window. |

With `"fullscreen"`, switching away from the fullscreen workspace, leaving
fullscreen, or closing the window disables VRR again.

A focused window can override this output policy with the window-rule `vrr`
key. See [window rules](window-rules.md#settings-updated-while-a-window-is-open).

```toml
[output.DP-1]
mode = "2560x1440@165"
vrr = "fullscreen"
```

Umbriel logs a warning and keeps VRR disabled if the output does not support
adaptive sync or rejects the request. Nested Wayland outputs normally depend on
the parent compositor and may not expose adaptive sync support.

### Tearing

Tearing is disabled by default and must be enabled per output:

```toml
[output.DP-1]
tearing = true
```

This setting is a safety gate, not an instruction to tear every frame. Umbriel
requests an asynchronous page flip only when the active workspace contains a
mapped fullscreen window and either the client requests asynchronous
presentation through the tearing-control protocol or a matching window rule
sets `tearing = true`. A window rule with `tearing = false` vetoes the client
hint. No window rule can bypass the output gate.

Umbriel temporarily uses regular page flips while the session is locked, an
overview or compositor confirmation overlay is visible, an output animation is
active, or the output is being captured. HDR, VRR, composition, and direct
scanout are not unconditional blockers. Umbriel tests the complete output state
with the backend and falls back to a regular page flip if the asynchronous
state is rejected or fails.

Backend support is required. Nested and headless outputs can exercise the
policy and protocol without proving that physical tearing occurs. Run `umbriel
tearing` to inspect the client hint, resolved rule, eligibility, last submitted
page-flip mode, presentation result, and any fallback reason. Use `umbriel
tearing --json` for machine-readable diagnostics.

See [window rules](window-rules.md#settings-updated-while-a-window-is-open)
for per-window overrides.

### HDR

HDR accepts these policies:

| Value          | Behavior                                                                                                                                                                                         |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `"off"`        | Keep the output in its normal SDR mode. This is the default.                                                                                                                                     |
| `"on"`         | Keep the output in PQ and BT.2020 continuously. SDR surfaces are mapped to `sdr_white`.                                                                                                          |
| `"auto"`       | Enable PQ and BT.2020 while a fullscreen surface with supported HDR metadata is visible on the active workspace. This includes PQ with BT.2020 and Wine's Windows scRGB or BT.2100 descriptions. |
| `"fullscreen"` | Enable PQ and BT.2020 while any fullscreen surface is visible on the active workspace.                                                                                                           |

Automatic HDR tracks the fullscreen surface that triggered the transition.
Other applications that adopt the HDR output color space after activation do
not keep HDR enabled. Leaving fullscreen, changing workspace, moving the
surface to another output, unmapping it, or closing it returns the output to
SDR.

Automatic HDR follows metadata committed by the client, including metadata on
mapped subsurfaces used by native Wayland Wine. It cannot infer a color space
from pixel values. Direct XWayland games and other clients that do not attach
color-management metadata remain undetectable. Use a native Wayland HDR path or
`hdr = "on"` for those clients. Automatic activation also requires fullscreen
content on the active workspace; windowed HDR content does not activate the
output.

When built with wayland-protocols 1.49 or newer, Umbriel exposes the predefined
Windows BT.2100 description to native Wayland Wine clients. Wine may instead
choose Windows scRGB for an HDR game. Both descriptions qualify for automatic
HDR.

Some native Wayland Wine builds require a runtime-specific launch option before
they publish HDR metadata. With Proton-CachyOS, use `DXVK_HDR=1` instead of
`PROTON_ENABLE_HDR=1`. Other Proton variants may behave differently; follow the
documentation for the selected compatibility tool.

A Steam launch option scopes the variable to one game. To publish it to newly
started systemd session services and their applications instead, configure it
for the Umbriel session:

```toml
[environment]
PROTON_ENABLE_WAYLAND = "1"
DXVK_HDR = "1"
```

This requires an Umbriel restart. Fully exit and relaunch Steam afterward,
because an existing Steam process keeps the environment with which it started.

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
PQ HDR output encoding requires fragment `highp` precision to preserve smooth
gradients in the 10-bit output. OpenGL ES 2 implementations without fragment
`highp` cannot initialize Umbriel's output shader.

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
restores the monitor exactly as it was. Tiled windows retain their order,
grouping, split ratios, and sizes in the scrolling, dwindle, and master layouts.
Its active workspace and the positions of floating, pinned, and scratchpad
windows return too. A disabled output is never picked as a focus, placement, or
layer-surface target.

```toml
[output.HDMI-A-1]
enabled = false
```

Changing `enabled` applies on the next config reload, like the other output
settings. Only the config file can disable an output; see below.

## Display power management

Use `dpms-off` and `dpms-on` to power configured monitors off and on without
removing them from the output layout or moving their workspaces and windows.
The bare actions target every configured output. Add a connector or monitor
name to target one monitor:

```sh
umbriel msg dpms-off
umbriel msg dpms-off:DP-1
umbriel msg dpms-on:DP-1
```

When every configured output is DPMS-off, a new keyboard or button press,
pointer or touch motion, wheel input, gesture activity, or tablet activity
powers all of them back on. Releases, repeated keybind actions, and gesture
end events do not wake outputs on their own, so the trailing release from a
`dpms-off` key or button cannot immediately undo it.

If another configured output remains powered, input activity leaves a named
DPMS-off output off. Use `dpms-on:<output>` to power that monitor back on.
Outputs disabled with `enabled = false` remain disabled and are not affected
by these actions.

## Live reconfiguration

Umbriel implements `wlr-output-management-unstable-v1`, so tools such as
`wlr-randr`, `kanshi`, and `wdisplays` can query and change mode, position,
scale, transform, and adaptive sync at runtime without editing the config file.
`umbriel outputs` only reads from this protocol; it does not send configuration
requests itself.

Requests that disable an output through this protocol are rejected: the
protocol commit would bypass the layout and overview handling that the config
`enabled` key performs. Use `enabled = false` instead.

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

The primary output is 3072 logical units wide (`3840 / 1.25`), so the HDMI
output starts at x = 3072.

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
