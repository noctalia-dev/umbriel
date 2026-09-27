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

Without a matching output section, Umbriel enables outputs that advertise a
preferred mode, a display identity, or no fixed mode list. A connector that
advertises modes but provides neither a preferred mode nor an identity stays
disabled. This avoids activating stale connector state reported by some DRM
drivers. Add a matching output section with `enabled = true` to enable such a
display explicitly.

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
| `bit_depth` | int | `8` | Render bit depth for SDR output: `8` or `10`. |
| `workspaces` | int, string array, or `"dynamic"` | `"dynamic"` | Workspace inventory for this output. |
| `min_workspaces` | int | `1` | Minimum count for a dynamic output. |
| `cyclic_workspaces` | bool | `false` | Wrap a workspace step around the ends of the inventory. |
| `workspace_axis` | string | `"vertical"` | Workspace arrangement axis. |
| `layout.scrolling.default_extent_fraction` | float | inherited | Initial scrolling-column extent on this output. |
| `screen_effect` | string | inherited | Replace `effects.screen` by name, or `"off"` to disable it on this output. |

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

### Cyclic workspaces

With `cyclic_workspaces = true`, a workspace step past either end of the
inventory wraps to the other end:

```toml
[output.DP-1]
workspaces = 3
cyclic_workspaces = true
```

This applies to `workspace-next`/`previous`,
`window-move-to-workspace-next`/`previous`,
`window-move-to-workspace-silent-next`/`previous`,
`window-move-or-workspace-up`/`down` at the column edge, and
`column-move-to-workspace-next`/`previous`.

On a dynamic output, the trailing empty workspace is the last one. Stepping
forward from the last populated workspace enters it, and one more step wraps to
the first. A static inventory wraps directly at both ends.

### Initial scrolling width

Override the global starting width for new scrolling columns on one output:

```toml
[output.DP-1.layout.scrolling]
default_extent_fraction = 0.4
```

A matching workspace rule can override this value. Reloading affects new
columns only; existing columns keep their current width. See
[Scrolling behavior](layout.md#scrolling-behavior).

### Screen effect

```toml
[output."HDMI-A-1"]
screen_effect = "off"
```

`screen_effect` names an `[effects.preset.<name>]` of kind `screen`, or `"off"`
to disable `effects.screen` on this output. See
[Effects](effects.md#turn-a-default-off-for-one-window-or-output).

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

### Bit depth

Set `bit_depth = 10` to request a 10-bit SDR compositor render format:

```toml
[output.DP-1]
bit_depth = 10
```

Umbriel selects XR30 (`DRM_FORMAT_XRGB2101010`) or XB30
(`DRM_FORMAT_XBGR2101010`) when the backend accepts it. XB30 is tried first if
it is already active. Otherwise, XR30 is tried first. If no 10-bit format
commits, the output falls back to 8-bit. HDR uses 10-bit independently of this
setting.

While a 10-bit format is active, blur and effects intermediate buffers are
upgraded to FP16 precision, provided the renderer supports FP16 render targets
and linear filtering of half-float textures. Otherwise, they remain 8-bit.

`bit_depth = 10` controls the compositor render format only. It does not
guarantee that the physical display link runs at 10 bits per channel. The
number of bits delivered to the panel depends on the display's EDID, cable,
and driver. Run `umbriel color` to confirm the active render format that the
compositor committed.

In `umbriel color --json`, `bit_depth` is the configured value and
`bit_depth_active` reports whether an enabled SDR output is currently using
XR30 or XB30. `bit_depth_fallback_reason` explains a failed 10-bit request.
It is empty while HDR is active.

#### VRR fallback

When VRR is also requested and the output supports adaptive sync, Umbriel tests
the formats with VRR first, in the order described above. If neither passes, it
tests them without VRR in the same order. Once a format passes its test,
Umbriel attempts to commit it. If that commit fails with VRR, it retries the
same format without VRR, without another test. A failed commit does not try the
other format. If no 10-bit format commits, it falls back to 8-bit. HDR follows
the same retry rule before falling back to SDR.

#### Direct scanout with 10-bit

Direct scanout remains enabled by the `direct_scanout` setting, but it may be
less likely to engage while 10-bit rendering is active. Direct scanout requires
the client buffer format to exactly match what KMS accepts for the plane. Set
`direct_scanout = false` to disable direct scanout for an output entirely.

#### Screencopy and capture

Screencopy clients such as `grim` and Noctalia receive raw buffers in the
output's active 10-bit render format (XR30 or XB30) when 10-bit SDR is active.
Unlike HDR capture, the pixels are not converted to an 8-bit SDR format first.
Tools that do not handle 10-bit formats may produce undesired output.

## Disabling an output

Set `enabled = false` for a persistent disabled state:

```toml
[output.HDMI-A-1]
enabled = false
```

The output leaves the desktop, but its workspaces and windows are retained and
return when it is enabled again. Output-management tools can temporarily
override this state until a later configuration reload reapplies the file.

Use the logical output actions for the same temporary change without an
external output-management tool:

```sh
umbriel msg output-disable:eDP-1
umbriel msg output-enable:eDP-1
umbriel msg output-toggle:eDP-1
```

These actions remove and restore the output as part of the desktop layout.
Windows move to another enabled output while their home is unavailable, then
return when it is enabled again. A disabled output is also absent from
whole-desktop screenshots. The actions can be used directly by
[lid event commands](configuration.md#events).

## Display power management

Use DPMS actions to power monitors off without removing their workspaces:

```sh
umbriel msg dpms-off
umbriel msg dpms-off:DP-1
umbriel msg dpms-on:DP-1
```

The bare actions target every configured output. Input wakes all monitors when
every output is powered off. Outputs disabled with `enabled = false` are not
affected. DPMS does not remove an output from the logical desktop, move its
windows, or exclude it from a whole-desktop capture.

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
