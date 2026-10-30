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

## Settings

| Key                                        | Type                              | Default      | Description                                                                                           |
| ------------------------------------------ | --------------------------------- | ------------ | ----------------------------------------------------------------------------------------------------- |
| `enabled`                                  | bool                              | `true`       | Turn the monitor on or off.                                                                           |
| `focus_at_startup`                         | bool                              | `false`      | Move the mouse cursor to this monitor on startup.                                                     |
| `mode`                                     | string                            | preferred    | Resolution and optional refresh rate, such as `"2560x1440@165"`.                                      |
| `position`                                 | `[x, y]`                          | automatic    | Top-left position in logical coordinates.                                                             |
| `scale`                                    | float                             | `1.0`        | Output scale from 0.25 to 4.0.                                                                        |
| `render_scale`                             | float                             | unset        | Scale clients render at (0.25 to 4.0); unset follows `scale`. The compositor samples down to `scale`. |
| `transform`                                | string                            | `"normal"`   | Rotation or reflection.                                                                               |
| `vrr`                                      | string                            | `"disabled"` | Variable refresh rate policy.                                                                         |
| `tearing`                                  | bool                              | `false`      | Allow eligible fullscreen windows to use asynchronous page flips.                                     |
| `direct_scanout`                           | bool                              | `true`       | Allow eligible fullscreen buffers to bypass composition.                                              |
| `hdr`                                      | string                            | `"off"`      | HDR activation policy.                                                                                |
| `sdr_white`                                | float                             | `203`        | SDR reference white in cd/m² while HDR is active.                                                     |
| `sdr_transfer`                             | string                            | `"gamma22"`  | Transfer curve for untagged SDR on HDR outputs: `"gamma22"` or `"srgb"`.                              |
| `bit_depth`                                | int                               | `8`          | Render bit depth for SDR output: `8` or `10`.                                                         |
| `workspaces`                               | int, string array, or `"dynamic"` | `"dynamic"`  | Workspace inventory for this output.                                                                  |
| `min_workspaces`                           | int                               | `1`          | Minimum count for a dynamic output.                                                                   |
| `cyclic_workspaces`                        | bool                              | `false`      | Wrap a workspace step around the ends of the inventory.                                               |
| `workspace_axis`                           | string                            | `"vertical"` | Workspace arrangement axis.                                                                           |
| `layout.scrolling.default_extent_fraction` | float                             | inherited    | Initial scrolling-column extent on this output, 0.1 to 4.0.                                           |
| `screen_effect`                            | string                            | inherited    | Replace `effects.screen` with a screen preset or pool, or `"off"` to disable it on this output.       |

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

### Initial scrolling width

Override the global starting width for new scrolling columns on one output:

```toml
[output.DP-1.layout.scrolling]
default_extent_fraction = 0.4
```

A matching workspace rule can override this value. Reloading affects new columns only; existing columns keep their current width. See [Scrolling behavior](layout.md#scrolling-behavior).

### Screen effect

```toml
[output."HDMI-A-1"]
screen_effect = "off"
```

`screen_effect` names an `[effects.preset.<name>]` or `[effects.pool.<name>]` of kind `screen`, or `"off"` to disable `effects.screen` on this output. See [Effects](effects.md#turn-a-default-off-for-one-window-or-output).

### Position and scale

Positions use logical coordinates after scale and transform. A `3840x2160` output at scale `1.25` occupies `3072x1728` logical units. An output immediately to its right therefore starts at x = 3072.

Omit `position` to place outputs automatically from left to right. Explicitly positioned outputs must touch or overlap for the pointer to move between them.

### Supersampling with `render_scale`

`render_scale` separates what things look like from what clients render at, the way macOS handles HiDPI. With `scale = 1.35` and `render_scale = 2.0`, clients are asked to render at 2.0 and the compositor samples their buffers down to the output's `scale` raster, which smooths text and fine detail on low-ppi and non-RGB-subpixel panels. Logical geometry is unchanged: windows keep their `scale` sizes, and the output still advertises `scale` so client interfaces size as before.

Applications that ignore the scale hints render as they always have. Values below `scale` are honored but undersample, which looks blurry. The configuration warning compares `render_scale` against the configured `scale` (1.0 when `scale` is unset), so it cannot see an auto-detected output scale. Hint-following buffers sized at `render_scale` no longer match the output mode while `render_scale` differs from `scale`, so they do not qualify for [direct scanout](#direct-scanout); set `render_scale` equal to `scale` for scanout-sensitive games.

Larger client buffers cost memory and sampling work: at `render_scale = 2.0` with `scale = 1.35` they hold about 2.2 times as many pixels as at `scale` alone, growing with the square of the ratio. Prefer whole or half-integer values for crisp buffer ratios.

### Transform values

Accepted values are `normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180`, and `flipped-270`.

### Direct scanout

Direct scanout can reduce composition work for eligible fullscreen applications. Disable it if fullscreen content causes corruption, black frames, or flicker:

```toml
[output.DP-1]
direct_scanout = false
```

The change applies on reload. Disabling direct scanout can increase GPU use and power consumption.

Set `WLR_SCENE_DISABLE_DIRECT_SCANOUT=1` before starting Umbriel to disable direct scanout on every output.

`umbriel outputs` shows whether it engages:

```text
  Direct scanout: active (steam_app_223850 "3DMark Steel Nomad")
  Direct scanout: inactive (software cursor)
```

The listing reports the latest scene buffer accepted by the backend immediately. Rejected commits do not change it. This describes a successful commit, not confirmation that the display presented it.

`umbriel outputs --json` also reports `direct_scanout.commits`, `direct_scanout.scanout_commits`, and `direct_scanout.commit_seq`. The two counters count accepted scene buffers and the subset committed through direct scanout. They retain brief scanout episodes even if the next query finds the output compositing again. Counters survive reloads and power cycles, and reset when the compositor restarts or the output is destroyed and recreated. The sequence identifies the latest accepted scene-buffer commit in the backend's wrapping 32-bit commit sequence.

For fullscreen corruption investigations, save a baseline before launching the application and another snapshot while the problem is visible:

```sh
umbriel outputs --json > outputs-before-launch.json
# Launch the application and reproduce the problem.
umbriel outputs --json > outputs-corrupted.json
umbriel color --json > color-corrupted.json
```

Collect the second snapshot before opening an overlay or switching workspaces. If `scanout_commits` increased, the backend accepted direct scanout commits on that output during the interval even if `active` is now false. An increase alone does not identify which window used scanout or establish the cause of corruption. Use a separate directory for each run when comparing scanout enabled and disabled before application startup, keeping HDR and the application arguments unchanged.

The log records each time scanout starts, stops, or moves to another window, once the new state has held for half a second:

```text
[INF] [output] output 'DP-1': direct scanout on (steam_app_223850 "3DMark Steel Nomad")
[INF] [output] output 'DP-1': direct scanout off (composited)
```

Scanout-triggered subscription notifications use the same settling interval. Counter increments alone do not push events; query `outputs` for immediate diagnostics.

When it is inactive, the reason is the first condition that ruled it out:

| Reason             | Meaning                                                                                                                        |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------ |
| `disabled`         | `direct_scanout = false`, or `WLR_SCENE_DISABLE_DIRECT_SCANOUT` is set.                                                        |
| `no_candidate`     | Nothing is on the output, or what is visible is not a client buffer.                                                           |
| `composited`       | More than one surface is visible: overlays, popups, borders, a translucent window, or a window that does not cover the output. |
| `effects`          | A screen, window, or animation effect is drawing on the output.                                                                |
| `color_conversion` | A gamma table, color transform, HDR white point, or the window's color description needs conversion.                           |
| `capture`          | An HDR output is being captured as SDR.                                                                                        |
| `cropped`          | The window is clipped, for instance mid-animation.                                                                             |
| `transform`        | The buffer's transform differs from the output's.                                                                              |
| `modeset`          | The output is changing mode or format this frame.                                                                              |
| `render_locked`    | A capture or animation needs the output composited.                                                                            |
| `software_cursor`  | The cursor is drawn in software over the window.                                                                               |
| `rejected`         | The display driver refused the buffer, for instance its format or modifier.                                                    |
| `debug_damage`     | Damage highlighting is on.                                                                                                     |

### Variable refresh rate

`vrr` accepts:

| Value          | Behavior                                                      |
| -------------- | ------------------------------------------------------------- |
| `"disabled"`   | Never enable adaptive sync.                                   |
| `"always"`     | Keep adaptive sync enabled when supported.                    |
| `"fullscreen"` | Enable it while the active workspace has a fullscreen window. |

```toml
[output.DP-1]
vrr = "fullscreen"
```

A focused window can override this policy through a [window rule](window-rules.md#settings-updated-while-a-window-is-open). Unsupported outputs remain at fixed refresh and produce a warning.

### Tearing

Tearing requires an output-level opt-in:

```toml
[output.DP-1]
tearing = true
```

Umbriel uses asynchronous presentation only for an eligible fullscreen window that requests it or matches a `tearing = true` window rule. A window rule can also veto a client request. Run `umbriel tearing` to inspect eligibility and fallback reasons.

### HDR

`hdr` accepts:

| Value          | Behavior                                                       |
| -------------- | -------------------------------------------------------------- |
| `"off"`        | Keep the output in SDR.                                        |
| `"on"`         | Keep the output in HDR.                                        |
| `"auto"`       | Enable HDR for fullscreen content with supported HDR metadata. |
| `"fullscreen"` | Enable HDR for any fullscreen content.                         |

```toml
[output.DP-1]
hdr = "auto"
sdr_white = 203
```

While HDR is active, `sdr_white` sets the brightness of composed SDR content. It also sets the reference white of the HDR encoding advertised to Wayland applications through output descriptions and surface color preferences. Reloading the setting updates those descriptions and notifies applications. Outputs offering only SDR retain the nominal 80 cd/m² reference white.

On a supported output, `hdr = "auto"` advertises an HDR encoding before the display switches to HDR. This lets native Wayland Wine and Proton detect HDR support before submitting their first HDR frame. The output remains in SDR until fullscreen content supplies HDR metadata. The advertised target color volume continues to describe the current SDR or HDR destination, and `umbriel color` reports the physical output's active state.

An HDR encoding can carry SDR content. In automatic mode, a positive content peak (MaxCLL) at or below the surface's reference white keeps the output in SDR, unless the application also supplies HDR mastering luminance. This keeps SDR browser content from switching the monitor to HDR merely because the browser uses a PQ buffer. A zero or missing content peak remains unknown; Umbriel then uses the description's encoding and luminance headroom.

HDR availability requires a successful backend test of an HDR output state. If activation fails, Umbriel withdraws the HDR encoding until an output configuration or mode change tests support again. Windowed HDR content and content whose activation fails receive highlight compression into SDR instead of clipping every bright highlight to white. Untagged SDR content keeps its normal rendering.

On HDR outputs, untagged SDR windows and compositor colors use gamma 2.2 by default to preserve their legacy SDR appearance. Set `sdr_transfer = "srgb"` to decode them with the piecewise sRGB curve instead. This produces lighter dark shades and can match applications that convert SDR into PQ using that curve:

```toml
[output.DP-1]
hdr = "on"
sdr_transfer = "srgb"
```

The choice applies on reload and only while that output uses HDR (PQ). It also keeps effect color controls and SDR capture encoding consistent with the chosen curve. Explicit client color descriptions keep their declared transfer function, including piecewise sRGB, gamma 2.2, PQ, and linear content. The display's PQ encoding and normal SDR rendering stay unchanged. Run `umbriel color` to inspect each output's configured `sdr_transfer` choice.

`sdr_white` can also be selected for one running output without editing the configuration:

```sh
umbriel msg output-sdr-white 203
umbriel msg output-sdr-white 120 --output DP-1
```

The preferred output, or the named one, keeps the value until it disconnects or a configuration reload changes output state, which restores the configured `sdr_white`. `umbriel color` reports the selected value.

Automatic HDR depends on metadata supplied by the application. Untagged XWayland content cannot be detected; use a native Wayland HDR path or `hdr = "on"` when necessary. Many monitors briefly go black while switching between SDR and HDR.

Native Wayland applications and Wine receive the same standard `wp_color_manager_v1` interface, including Windows scRGB, explicit primaries and luminance ranges. Nested Gamescope can use its HDR and HDR inverse tone-mapping options through this interface with either PQ or scRGB buffers. No additional Umbriel option or layer rule is required. The output must support HDR, and Gamescope's own HDR settings must be enabled. Automatic mode offers an HDR encoding during startup, while clients that require an already active HDR target need `hdr = "on"`. A separate frog protocol is unnecessary.

Windows scRGB uses absolute brightness: a linear sample of 1.0 represents 80 cd/m² on an HDR output, including when `sdr_white` changes. Generic parametric linear content remains relative to its declared reference white. Captures, overview previews and window snapshots preserve the client's color description.

Some native Wayland Proton builds require `PROTON_ENABLE_WAYLAND=1` and `DXVK_HDR=1` before they publish HDR metadata. Proton variants differ, so follow the selected runtime's documentation and fully restart Steam after changing session environment values.

On a native DRM output, Umbriel reads the display's CTA HDR static metadata from its EDID. The display primaries, desired minimum and maximum content luminance, and desired maximum frame-average luminance are included in the BT.2020 PQ output description. This gives the display a valid mastering range, MaxCLL, and MaxFALL for its own tone mapping. Umbriel leaves any value omitted by the EDID undefined instead of guessing a brightness that may be wrong for the panel. Nested and virtual outputs have no display EDID, so their luminance metadata remains undefined.

These display limits describe the target color volume independently of `sdr_white`, which describes SDR reference white within the HDR encoding.

Run `umbriel color` to inspect the metadata in the active output description. The JSON form reports `mastering_display_primaries`, `mastering_luminance`, `max_cll`, and `max_fall` for each output. For surfaces with explicit primary coordinates, `primaries` reports `"custom"`.

Shared-memory screenshots from normal screencopy and image-copy clients receive an 8-bit SDR view while HDR is active. DMA-BUF capture may retain 10-bit storage while still containing SDR values. Raw export-DMA-BUF is not advertised. Use screencopy or image-copy clients, including image-copy clients with DMA-BUF destinations.

Window captures, including portal window previews, honor the client's committed color metadata and convert HDR client content to SDR.

Existing gamma-control clients, including Night Light tools, can adjust white point and brightness while HDR remains enabled. Umbriel uses each gamma ramp's white endpoint to adjust the image in linear light before PQ encoding. This retains HDR highlight headroom instead of clipping highlights to SDR white, though warming the image deliberately changes its colors and channel brightness.

Custom gamma-curve calibration applies only in SDR. Umbriel keeps the client's latest full gamma table and restores it when the output returns to SDR. An HDR white-point adjustment applies to the whole display, including the cursor, and requires composition while active. SDR screenshots and recordings exclude this adjustment.

### Bit depth

Set `bit_depth = 10` to request a 10-bit SDR compositor render format:

```toml
[output.DP-1]
bit_depth = 10
```

Umbriel selects XR30 (`DRM_FORMAT_XRGB2101010`) or XB30 (`DRM_FORMAT_XBGR2101010`) when the backend accepts it. XB30 is tried first if it is already active. Otherwise, XR30 is tried first. If no 10-bit format commits, the output falls back to 8-bit. HDR uses 10-bit independently of this setting.

While a 10-bit format is active, blur and effects intermediate buffers are upgraded to FP16 precision, provided the renderer supports FP16 render targets and linear filtering of half-float textures. Otherwise, they remain 8-bit.

`bit_depth = 10` controls the compositor render format only. It does not guarantee that the physical display link runs at 10 bits per channel. The number of bits delivered to the panel depends on the display's EDID, cable, and driver. Run `umbriel color` to confirm the active render format that the compositor committed.

In `umbriel color --json`, `bit_depth` is the configured value and `bit_depth_active` reports whether an enabled SDR output is currently using XR30 or XB30. `bit_depth_fallback_reason` explains a failed 10-bit request. It is empty while HDR is active.

#### VRR fallback

When VRR is also requested and the output supports adaptive sync, Umbriel tests the formats with VRR first, in the order described above. If neither passes, it tests them without VRR in the same order. Once a format passes its test, Umbriel attempts to commit it. If that commit fails with VRR, it retries the same format without VRR, without another test. A failed commit does not try the other format. If no 10-bit format commits, it falls back to 8-bit. HDR follows the same retry rule before falling back to SDR.

#### Direct scanout with 10-bit

Direct scanout remains enabled by the `direct_scanout` setting, but it may be less likely to engage while 10-bit rendering is active. Direct scanout requires the client buffer format to exactly match what KMS accepts for the plane. Set `direct_scanout = false` to disable direct scanout for an output entirely.

#### Screencopy and capture

Shared-memory captures through screencopy and image-copy use an 8-bit format even when the output render format is XR30 or XB30. This applies to both 10-bit SDR outputs and the SDR capture view produced while HDR is active, improving compatibility with clients whose shared-memory paths assume 8-bit pixels. DMA-BUF capture may retain the negotiated 10-bit storage format. During HDR, that buffer still contains Umbriel's SDR capture view. Raw export-DMA-BUF is not available.

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
