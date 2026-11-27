# Outputs

Output sections configure monitors by connector name, such as `DP-1`, or by monitor identity:

```toml
[output.DP-1]
mode = "3840x2160@165"
position = [0, 0]
scale = 1.25
focus_at_startup = true
```

Run `umbriel outputs` inside a session to list names and available modes. The `Config name` value is a copyable monitor identity in `"<make> <model> <serial>"` form:

```toml
[output."Microstep MSI G2712F CD6T084401192"]
mode = "1920x1080@180"
```

Use a monitor identity when settings should follow one display between ports. Use a connector when settings belong to a physical port. If both match, the monitor section wins. Matching is case-insensitive.

Without a matching output section, Umbriel enables outputs that advertise a preferred mode, a display identity, or no fixed mode list. A connector that advertises modes but provides neither a preferred mode nor an identity stays disabled. This avoids activating stale connector state reported by some DRM drivers. Add a matching output section with `enabled = true` to enable such a display explicitly.

When an output disconnects or is disabled, each populated workspace moves into its own temporary workspace on another enabled output. Umbriel appends these workspaces without replacing that output's configured workspaces, including when its workspace inventory is fixed. Temporary workspaces use numeric labels for originally anonymous workspaces; named workspaces keep their explicit names, so name-based keybinds continue to work. Both retain their original layout rules. When the same output connector becomes available again, their layout and positions are restored there. Windows opened in a temporary workspace return with it; explicitly moving a window into a local workspace cancels its automatic return. Tiling edits made in a temporary workspace also return with it when its tiled windows remain mapped.

If a receiving output already has a workspace with the same name, its native workspace keeps priority for name lookup. Qualify the displaced name with its original output, such as `workspace-switch WEB --output DP-1`, to select it while `DP-1` is unavailable. See [workspace selectors](workspaces.md#workspace-selectors).

Umbriel first uses the first remaining enabled output in its internal output order. If that output has reached the 64-workspace limit, it tries another enabled output. If every enabled output is full, displaced windows share a local workspace until their home returns. Growing a fixed inventory during the disconnection can also leave too little room for temporary workspaces; those windows retain their saved homes. Empty temporary workspaces are removed. If every output disappears, windows wait until an output returns, then recover their separate workspaces there while their home output is still absent.

## VR displays

On a native DRM session, Umbriel offers displays marked as non-desktop by the graphics driver through the standard `drm-lease-v1` protocol. A VR runtime can take direct control of the headset display while Umbriel continues to manage ordinary monitors. No headset-specific configuration is needed.

Non-desktop displays do not receive workspaces and do not appear in `umbriel outputs`. They stay outside the desktop even if leasing is unavailable. Displays without the driver's non-desktop designation remain ordinary desktop outputs.

Locking the session or switching away from its active VT revokes outstanding leases. The VR runtime must acquire a new lease after the session is unlocked and active again; restarting the runtime may be necessary.

## Settings

| Key                                        | Type                              | Default      | Description                                                                                           |
| ------------------------------------------ | --------------------------------- | ------------ | ----------------------------------------------------------------------------------------------------- |
| `enabled`                                  | bool                              | `true`       | Turn the monitor on or off.                                                                           |
| `focus_at_startup`                         | bool                              | `false`      | Move the mouse cursor to this monitor on startup.                                                     |
| `mode`                                     | string                            | preferred    | Resolution and optional refresh rate, such as `"2560x1440@165"`.                                      |
| `position`                                 | `[x, y]`                          | automatic    | Top-left position in logical coordinates.                                                             |
| `scale`                                    | float                             | `1.0`        | Output scale from 0.25 to 4.0.                                                                        |
| `render_scale`                             | float                             | unset        | Scale clients render at (0.25 to 4.0); unset follows `scale`. The compositor samples down to `scale`. |
| `supersample_fullscreen`                   | bool                              | `false`      | Apply `render_scale` to fullscreen windows, subject to content type and window rules.                 |
| `transform`                                | string                            | `"normal"`   | Rotation or reflection.                                                                               |
| `vrr`                                      | string                            | `"disabled"` | Variable refresh rate policy.                                                                         |
| `tearing`                                  | bool                              | `false`      | Allow eligible fullscreen windows to use asynchronous page flips.                                     |
| `direct_scanout`                           | bool                              | `true`       | Allow eligible fullscreen buffers to bypass composition.                                              |
| `hdr`                                      | string                            | `"off"`      | HDR activation policy.                                                                                |
| `sdr_white`                                | float                             | `203`        | SDR reference white in cd/m² while HDR is active, from 80 to 1000.                                    |
| `sdr_transfer`                             | string                            | `"gamma22"`  | Transfer curve for untagged SDR on HDR outputs: `"gamma22"` or `"srgb"`.                              |
| `color_profile`                            | string                            | `"none"`     | SDR panel characterization: `"none"`, `"edid"`, or a path to an ICC profile.                          |
| `sdr_gamut`                                | string                            | `"native"`   | SDR gamut policy while a characterization is active: `"native"` or `"srgb"`.                          |
| `bit_depth`                                | int                               | `8`          | Render bit depth for SDR output: `8` or `10`.                                                         |
| `workspaces`                               | int, string array, or `"dynamic"` | `"dynamic"`  | Workspace inventory for this output.                                                                  |
| `min_workspaces`                           | int                               | `1`          | Minimum count for a dynamic output.                                                                   |
| `cyclic_workspaces`                        | bool                              | `false`      | Wrap a workspace step around the ends of the inventory.                                               |
| `workspace_axis`                           | string                            | `"vertical"` | Workspace arrangement axis.                                                                           |
| `layout.mode`                              | string                            | inherited    | Layout mode for workspaces on this output: `"scrolling"`, `"dwindle"`, `"master"`, or `"stacking"`.   |
| `layout.scrolling.default_extent_fraction` | float                             | inherited    | Initial scrolling-column extent on this output, 0.1 to 4.0.                                           |
| `screen_effect`                            | string                            | inherited    | Replace `effects.screen` with a screen preset or pool, or `"off"` to disable it on this output.       |
| `screen_cast_effect`                       | string                            | inherited    | Replace `effects.screen_cast` with a screen preset or pool, or `"off"` to disable it on this output.  |

Umbriel tries an unadvertised resolution as a custom mode. If it cannot apply the configured mode, it uses the preferred advertised mode and logs a warning. Backend tests include the 8-bit SDR fallback, so a rejected swapchain can reach the preferred mode. On that mode, Umbriel retries the configured HDR and bit-depth preferences. A temporary rendering failure after an accepted backend test retries the current candidate instead of changing modes.

### Workspace count

`workspaces` accepts:

- `"dynamic"` or an omitted value for workspaces that grow and shrink
- An integer for a fixed number of anonymous workspaces
- A string array for a fixed ordered list of names

[Workspace transfers](workspaces.md#moving-a-workspace-between-outputs) can grow or shrink a fixed inventory for the current session. Changing workspace inventory configuration reapplies the configured inventory.

`min_workspaces` sets a floor for a dynamic output:

```toml
[output.DP-1]
min_workspaces = 3
```

Do not combine `min_workspaces` with a fixed workspace inventory. See [Workspaces](workspaces.md#choose-a-workspace-model) for naming, lifecycle, and workspace rules.

### Cyclic workspaces

With `cyclic_workspaces = true`, a workspace step past either end of the inventory wraps to the other end:

```toml
[output.DP-1]
workspaces = 3
cyclic_workspaces = true
```

This applies to `workspace-next`/`previous`, `window-move-to-workspace-next`/`previous`, `window-move-to-workspace-silent-next`/`previous`, `window-move-or-workspace-up`/`down` at the column edge, and `column-move-to-workspace-next`/`previous`.

On a dynamic output, the trailing empty workspace is the last one. Stepping forward from the last populated workspace enters it, and one more step wraps to the first. A static inventory wraps directly at both ends.

### Layout mode

Choose the layout mode for every workspace on one output, including dynamic workspaces created later:

```toml
[output.DP-1.layout]
mode = "dwindle"
```

A matching workspace rule overrides this value. The `mode` key directly under `[output.<name>]` is the display mode, not the layout. See [Choose a layout](layout.md#choose-a-layout).

### Initial scrolling width

Override the global starting width for new scrolling columns on one output:

```toml
[output.DP-1.layout.scrolling]
default_extent_fraction = 0.4
```

A matching workspace rule can override this value. Reloading affects new columns only; existing columns keep their current width. See [Scrolling behavior](layout-scrolling.md#scrolling-behavior).

### Screen effect

```toml
[output."HDMI-A-1"]
screen_effect = "off"
```

`screen_effect` names an `[effects.preset.<name>]` or `[effects.pool.<name>]` of kind `screen`, or `"off"` to disable `effects.screen` on this output. See [Effects](effects.md#turn-a-default-off-for-one-window-or-output).

`screen_cast_effect` does the same for `effects.screen_cast`, the screen effect that runs on this output only while a client captures it whole:

```toml
[output."DP-1"]
screen_cast_effect = "wisps"
```

### Position and scale

Positions use logical coordinates after scale and transform. A `3840x2160` output at scale `1.25` occupies `3072x1728` logical units. An output immediately to its right therefore starts at x = 3072.

Omit `position` to place outputs automatically from left to right. Explicitly positioned outputs must touch or overlap for the pointer to move between them.

### Supersampling with `render_scale`

`render_scale` separates what things look like from what Wayland clients render at, the way macOS handles HiDPI. With `scale = 1.35` and `render_scale = 2.0`, clients are asked to render at 2.0 and the compositor samples their buffers down to the output's `scale` raster, which smooths text and fine detail on low-ppi and non-RGB-subpixel panels. Logical geometry is unchanged: windows keep their `scale` sizes, and the output still advertises `scale` so client interfaces size as before.

Which windows render at `render_scale`:

| Window                                                                                       | Renders at                                                                 |
| -------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------- |
| Wayland window                                                                               | `render_scale`                                                             |
| Fullscreen Wayland window                                                                    | `scale` by default, or `render_scale` when `supersample_fullscreen = true` |
| Wayland window whose [content type](window-rules.md#identity-selectors) is `game` or `video` | `scale`                                                                    |
| X11 window, default                                                                          | unchanged: X11 applications ignore the scale hints                         |
| X11 window with [`force_x11_supersample`](configuration.md#general)                          | `render_scale`, fullscreen included; no window rule exempts it             |
| Wayland window with a `supersample` [window rule](window-rules-behavior.md#supersampling)    | `render_scale` with `true`, `scale` with `false`                           |

Supersampling a game or a video only costs GPU time, since both draw at a resolution of their own. Games therefore need attention only as Wayland windows that are neither fullscreen nor tagged `game`, such as a game in windowed mode. Exempt those by app ID; a list covers several at once:

```toml
[[window_rule]]
match.app_id = ["^my-game$", "^my-other-game$"]
supersample = false
```

Games that run through Xwayland need no rule while `force_x11_supersample` is off.

Fullscreen windows use `scale` by default, while windowed and maximized windows use `render_scale` unless their content type is `game` or `video`. For a 3D application such as Blender that does not advertise either content type, this can make windowed rendering more demanding and less responsive than fullscreen. Set `supersample_fullscreen = true` on an output to keep the higher rendering resolution in fullscreen as well. This increases fullscreen rendering cost and can prevent direct scanout. To reduce rendering cost in every mode for a particular application, use a `supersample = false` window rule instead. Window rules take precedence over both the fullscreen setting and content-type exemptions.

Layer surfaces supersample too, with no exemption of their own. A `supersample = false` [layer rule](layer-rules.md) keeps a surface and its popups at `scale`, for example a screenshot overlay that shows a frozen capture of the output, which supersampling would scale up and back down:

```toml
[[layer_rule]]
match.namespace = "^my-screenshot-overlay$"
supersample = false
```

Keep the default for layer surfaces that draw text or vector shapes, which gain from supersampling. A surface that already holds full-resolution pixels, such as a screenshot overlay, is the one to exempt. Applications that ignore the scale hints render as they always have. Values below `scale` are honored but undersample, which looks blurry. The configuration warning compares `render_scale` against the configured `scale` (1.0 when `scale` is unset), so it cannot see an auto-detected output scale. Hint-following buffers sized at `render_scale` no longer match the output mode while `render_scale` differs from `scale`, so they do not qualify for [direct scanout](#direct-scanout); fullscreen windows are exempt by default, as described above.

Larger client buffers cost memory and sampling work: at `render_scale = 2.0` with `scale = 1.35` they hold about 2.2 times as many pixels as at `scale` alone, growing with the square of the ratio. Sharpness depends on the ratio of `render_scale` to `scale`, not on `render_scale` alone.

### Transform values

Accepted values are `normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180`, and `flipped-270`.

### HDR, VRR, and tearing

The `direct_scanout`, `vrr`, `tearing`, `hdr`, `sdr_white`, `sdr_transfer`, `color_profile`, `sdr_gamut`, and `bit_depth` keys are described in [HDR, VRR, and tearing](display-features.md).

### Window capture resolution

Window screenshots and window sharing render at the window's current output scale. A fullscreen window on a 3200×2000 output at scale 2.5 therefore captures at 3200×2000 pixels, while its logical size remains 1280×800. Fractional pixel dimensions round up. Moving the window to an output with a different scale or changing its output's scale updates ongoing capture sessions' buffer sizes.

Larger capture buffers increase memory use, rendering and copying work, and may increase encoding work for window sharing. At scale 2.5 they contain 6.25 times as many pixels as a scale 1 capture of the same logical window. Applications that supply low-resolution buffers still have only that source detail. Idle capture sources do not render, and stopping the last session releases their output swapchain.

## Virtual outputs

A virtual output is a monitor with no display behind it. It has workspaces, takes windows, and can be captured like any other output, which makes it a target for remote desktop and game streaming. See [Game streaming](streaming.md) for a Sunshine setup.

```sh
umbriel output-create stream 2560x1440@120
umbriel output-destroy stream
```

The name uses ASCII letters, digits, `-`, `_`, and `.`, and must not match an existing output, ignoring case. `output-create` prints the new output's name. The optional mode takes the same `WIDTHxHEIGHT[@HZ]` form as the `mode` setting below and sets the size and frame pacing at creation; without it, a virtual output starts at 1280x720. An output section or an output-management tool such as `wlr-randr` changes the mode afterwards:

```toml
[output.stream]
mode = "1920x1080@60"
```

```sh
wlr-randr --output stream --custom-mode 2560x1440@120Hz
```

`output-destroy` accepts only virtual outputs. Its windows move to another output, as when a monitor is unplugged.

## Disabling an output

Set `enabled = false` for a persistent disabled state:

```toml
[output.HDMI-A-1]
enabled = false
```

The output leaves the desktop, but its workspaces and windows are retained and return when it is enabled again. Output-management tools can temporarily override this state until a later output-policy change in the configuration reapplies the file. Reloading identical content or changing an unrelated setting leaves the temporary state intact.

Use the logical output actions for the same temporary change without an external output-management tool:

```sh
umbriel msg output-disable eDP-1
umbriel msg output-enable eDP-1
umbriel msg output-toggle eDP-1
```

These actions remove and restore the output as part of the desktop layout. Windows move to another enabled output while their home is unavailable, then return when it is enabled again. A disabled output is also absent from whole-desktop screenshots. The actions can be used directly by [lid event commands](configuration.md#events).

A temporary enable or disable choice survives the output disappearing and returning during the same compositor session. For a monitor with display identity, the choice follows that monitor if it returns on another connector; a different identified monitor on the old connector does not inherit it. Outputs without display identity remain associated with their connector name.

## Display power management

Use DPMS actions to power monitors off without removing their workspaces:

```sh
umbriel msg dpms-off
umbriel msg dpms-off DP-1
umbriel msg dpms-on DP-1
```

The bare actions target every configured output. Input wakes all monitors when every output is powered off. Outputs disabled with `enabled = false` are not affected. DPMS does not remove an output from the logical desktop, move its windows, or exclude it from a whole-desktop capture.

## Live reconfiguration

Tools such as `wlr-randr`, `kanshi`, and `wdisplays` can change enabled state, mode, position, scale, transform, and adaptive sync while Umbriel is running. Those changes last until another tool request or a changed output-state policy in the configuration replaces them. An identical or unrelated reload is inert.

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

The primary output is 3072 logical units wide, so the HDMI output begins at x = 3072.

## Machine-specific overrides

Keep output configuration in a machine-specific include when sharing one base configuration between systems:

```toml
[include]
files = [
  "src/general.toml",
  "src/keybinds.toml",
  "machines/monolith.toml",
]
```

Each output keeps its screen pool assignment while disabled. Disconnecting releases it; reconnecting starts from configuration. Inspect assignments with `umbriel effects --json`. The cursor has one session-wide selection shared by all outputs.
