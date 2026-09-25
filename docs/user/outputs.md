# Outputs

Output sections configure monitors by connector name, such as `DP-1`, or by
monitor identity:

```toml
[output.DP-1]
mode = "3840x2160@165"
position = [0, 0]
scale = 1.25
```

Run `umbriel outputs` inside a session to list names and available modes. The
`Config name` value is a copyable monitor identity in
`"<make> <model> <serial>"` form:

```toml
[output."Microstep MSI G2712F CD6T084401192"]
mode = "1920x1080@180"
```

Use a monitor identity when settings should follow one display between ports.
Use a connector when settings belong to a physical port. If both match, the
monitor section wins. Matching is case-insensitive.

When an output disconnects or is disabled, Umbriel temporarily moves its
workspaces and windows to another enabled output. They return with their layout
and positions when the output becomes available again.

## Settings

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | bool | `true` | Turn the monitor on or off. |
| `mode` | string | preferred | Resolution and optional refresh rate, such as `"2560x1440@165"`. |
| `position` | `[x, y]` | automatic | Top-left position in logical coordinates. |
| `scale` | float | `1.0` | Output scale from 0.25 to 4.0. |
| `transform` | string | `"normal"` | Rotation or reflection. |
| `vrr` | string | `"disabled"` | Variable refresh rate policy. |
| `tearing` | bool | `false` | Allow eligible fullscreen windows to use asynchronous page flips. |
| `direct_scanout` | bool | `true` | Allow eligible fullscreen buffers to bypass composition. |
| `hdr` | string | `"off"` | HDR activation policy. |
| `sdr_white` | float | `203` | SDR reference white in cd/m² while HDR is active. |
| `workspaces` | int, string array, or `"dynamic"` | `"dynamic"` | Workspace inventory for this output. |
| `min_workspaces` | int | `1` | Minimum count for a dynamic output. |
| `workspace_axis` | string | `"vertical"` | Workspace arrangement axis. |
| `layout.scrolling.default_extent_fraction` | float | inherited | Initial scrolling-column extent on this output. |

Umbriel tries an unadvertised resolution as a custom mode. If it cannot apply
the configured mode, it uses the preferred advertised mode and logs a warning.

### Workspace count

`workspaces` accepts:

- `"dynamic"` or an omitted value for workspaces that grow and shrink
- An integer for a fixed number of anonymous workspaces
- A string array for a fixed ordered list of names

`min_workspaces` sets a floor for a dynamic output:

```toml
[output.DP-1]
min_workspaces = 3
```

Do not combine `min_workspaces` with a fixed workspace inventory. See
[Workspaces](workspaces.md#choose-a-workspace-model) for naming, lifecycle, and
workspace rules.

### Initial scrolling width

Override the global starting width for new scrolling columns on one output:

```toml
[output.DP-1.layout.scrolling]
default_extent_fraction = 0.4
```

A matching workspace rule can override this value. Reloading affects new
columns only; existing columns keep their current width. See
[Scrolling behavior](layout.md#scrolling-behavior).

### Position and scale

Positions use logical coordinates after scale and transform. A `3840x2160`
output at scale `1.25` occupies `3072x1728` logical units. An output immediately
to its right therefore starts at x = 3072.

Omit `position` to place outputs automatically from left to right. Explicitly
positioned outputs must touch or overlap for the pointer to move between them.

### Transform values

Accepted values are `normal`, `90`, `180`, `270`, `flipped`, `flipped-90`,
`flipped-180`, and `flipped-270`.

### Direct scanout

Direct scanout can reduce composition work for eligible fullscreen
applications. Disable it if fullscreen content causes corruption, black frames,
or flicker:

```toml
[output.DP-1]
direct_scanout = false
```

The change applies on reload. Disabling direct scanout can increase GPU use and
power consumption.

Set `WLR_SCENE_DISABLE_DIRECT_SCANOUT=1` before starting Umbriel to disable
direct scanout on every output.

### Variable refresh rate

`vrr` accepts:

| Value | Behavior |
| --- | --- |
| `"disabled"` | Never enable adaptive sync. |
| `"always"` | Keep adaptive sync enabled when supported. |
| `"fullscreen"` | Enable it while the active workspace has a fullscreen window. |

```toml
[output.DP-1]
vrr = "fullscreen"
```

A focused window can override this policy through a
[window rule](window-rules.md#settings-updated-while-a-window-is-open).
Unsupported outputs remain at fixed refresh and produce a warning.

### Tearing

Tearing requires an output-level opt-in:

```toml
[output.DP-1]
tearing = true
```

Umbriel uses asynchronous presentation only for an eligible fullscreen window
that requests it or matches a `tearing = true` window rule. A window rule can
also veto a client request. Run `umbriel tearing` to inspect eligibility and
fallback reasons.

### HDR

`hdr` accepts:

| Value | Behavior |
| --- | --- |
| `"off"` | Keep the output in SDR. |
| `"on"` | Keep the output in HDR. |
| `"auto"` | Enable HDR for fullscreen content with supported HDR metadata. |
| `"fullscreen"` | Enable HDR for any fullscreen content. |

```toml
[output.DP-1]
hdr = "auto"
sdr_white = 203
```

Automatic HDR depends on metadata supplied by the application. Untagged
XWayland content cannot be detected; use a native Wayland HDR path or
`hdr = "on"` when necessary. Many monitors briefly go black while switching
between SDR and HDR.

Some native Wayland Proton builds require `PROTON_ENABLE_WAYLAND=1` and
`DXVK_HDR=1` before they publish HDR metadata. Proton variants differ, so follow
the selected runtime's documentation and fully restart Steam after changing
session environment values.

Screenshots from normal screencopy clients receive an SDR view while HDR is
active.

## Disabling an output

Set `enabled = false` for a persistent disabled state:

```toml
[output.HDMI-A-1]
enabled = false
```

The output leaves the desktop, but its workspaces and windows are retained and
return when it is enabled again. Output-management tools can temporarily
override this state until a later configuration reload reapplies the file.

## Display power management

Use DPMS actions to power monitors off without removing their workspaces:

```sh
umbriel msg dpms-off
umbriel msg dpms-off:DP-1
umbriel msg dpms-on:DP-1
```

The bare actions target every configured output. Input wakes all monitors when
every output is powered off. Outputs disabled with `enabled = false` are not
affected.

## Live reconfiguration

Tools such as `wlr-randr`, `kanshi`, and `wdisplays` can change enabled state,
mode, position, scale, transform, and adaptive sync while Umbriel is running.
Those changes last until another tool request or an output-related config
reload replaces them.

## Multi-monitor example

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

The primary output is 3072 logical units wide, so the HDMI output begins at
x = 3072.

## Machine-specific overrides

Keep output configuration in a machine-specific include when sharing one base
configuration between systems:

```toml
[include]
files = [
  "src/general.toml",
  "src/keybinds.toml",
  "machines/monolith.toml",
]
```
