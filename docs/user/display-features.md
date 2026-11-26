# HDR, VRR, and tearing

These per-output settings trade composition work for latency, smoothness, or color range. Each goes in an output's table, as in `[output.DP-1]`; see [Outputs](outputs.md) for naming outputs and the other output settings.

## Direct scanout

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

## Variable refresh rate

`vrr` accepts:

| Value              | Behavior                                                                            |
| ------------------ | ----------------------------------------------------------------------------------- |
| `"disabled"`       | Never enable adaptive sync.                                                         |
| `"always"`         | Keep adaptive sync enabled when supported.                                          |
| `"fullscreen"`     | Enable it while the active workspace has a fullscreen window.                       |
| `"fullscreen_any"` | Enable it while any workspace on the output has a fullscreen window, active or not. |

```toml
[output.DP-1]
vrr = "fullscreen"
```

`"fullscreen_any"` keeps adaptive sync on while you switch from a fullscreen game to another workspace on the same output and back, so the output changes its VRR state only when the first window there enters fullscreen and when the last one leaves it or closes. `"fullscreen"` turns it off as soon as the active workspace holds no fullscreen window.

A focused window can override this policy through a [window rule](window-rules-behavior.md#behavior-keys). A window rule accepts `"disabled"`, `"always"`, or `"fullscreen"`, where `"fullscreen"` follows the focused window's own fullscreen state. Unsupported outputs remain at fixed refresh and produce a warning.

## Tearing

Tearing requires an output-level opt-in:

```toml
[output.DP-1]
tearing = true
```

Umbriel uses asynchronous presentation only for an eligible fullscreen window that requests it or matches a `tearing = true` window rule. A window rule can also veto a client request. Run `umbriel tearing` to inspect eligibility and fallback reasons.

## HDR

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

The choice applies on reload and only while that output uses HDR (PQ). It also keeps effect color controls and SDR capture encoding consistent with the chosen curve. Explicit client color descriptions keep their declared transfer function, including piecewise sRGB, gamma 2.2, PQ, and linear content. The display's PQ encoding and normal SDR rendering stay unchanged. Run `umbriel color` to inspect each output's configured `sdr_transfer` choice, which stays inert while the output renders SDR.

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

Run `umbriel color` to inspect the metadata in the active output description. The JSON form reports `mastering_display_primaries`, `mastering_luminance`, `max_cll`, and `max_fall` for each output. Its `transfer_function` and `primaries` fields describe the same HDR image description, so both read `"none"` while no HDR encoding is active. For surfaces with explicit primary coordinates, `primaries` reports `"custom"`.

Shared-memory screenshots from normal screencopy and image-copy clients receive an 8-bit SDR view while HDR is active. DMA-BUF capture may retain 10-bit storage while still containing SDR values. Raw export-DMA-BUF is not advertised. Use screencopy or image-copy clients, including image-copy clients with DMA-BUF destinations.

Window captures, including portal window previews, honor the client's committed color metadata and convert HDR client content to SDR.

Existing gamma-control clients, including Night Light tools, can adjust white point and brightness while HDR remains enabled. Umbriel uses each gamma ramp's white endpoint to adjust the image in linear light before PQ encoding. This retains HDR highlight headroom instead of clipping highlights to SDR white, though warming the image deliberately changes its colors and channel brightness.

Custom gamma-curve calibration applies only in SDR. Umbriel keeps the client's latest full gamma table and restores it when the output returns to SDR. An HDR white-point adjustment applies to the whole display, including the cursor, and requires composition while active. SDR screenshots and recordings exclude this adjustment.

## Color profile and SDR gamut

A wide-gamut panel reads every signal value in its own primaries. Untagged SDR content is sRGB, so the panel renders it more saturated than it should (vivid reds and greens), and a window that looks right on an sRGB monitor looks wrong here. Umbriel can characterize the display and convert during composition:

```toml
[output.DP-1]
color_profile = "edid"  # "none" (default), "edid", or a path to an ICC file
sdr_gamut = "srgb"      # "native" (default) or "srgb"
```

`color_profile` selects the characterization: an ICC file (the factory measurement) takes priority, then `"edid"` derived from the display's own metadata, then `"none"`. The default is `"none"`, which stages nothing. Profile paths follow these rules:

- A relative path resolves against the directory of the config file that sets it, include files included.
- `~` and `~/` expand to `$HOME`. No other variable placeholders are expanded.
- `"none"` and `"edid"` are reserved keywords. A file literally named `edid` (or `none`) must be spelled `./edid` (`./none`) or given as an absolute path.
- The path must resolve to a readable regular file. Directories, FIFOs, and device files are rejected.

Failures fall back down the chain and never fail the output. A value that is not a usable string (wrong type, empty, or containing a NUL byte) is ignored with a warning and the output keeps `"none"`; a path that no longer resolves to a readable regular file falls back to `"edid"` with a warning, and so does an ICC profile that cannot be parsed. Nested and headless outputs have no EDID, so they end up uncharacterized and gamut mapping stays inactive. Saving the profile file triggers the config reload that re-reads it; the re-read is keyed on the file's mtime, so replace the profile with a file whose timestamp actually changed (`cp -p` and archive extraction often keep the old one, and the previous characterization then stays live).

`sdr_gamut` decides what happens to untagged SDR while a characterization is active:

- `"native"` (the default) stages no transform. Pixels stay bit-identical to an uncharacterized output, so behavior only changes where you opted in.
- `"srgb"` interprets untagged SDR as sRGB and maps it into the panel's colorimetry: the oversaturation fix. The mapping is white-preserving, so switching between the two settings does not change white.

Applications with explicit color management follow the same split: while `sdr_gamut = "srgb"` is active, output and preferred descriptions advertise the panel's colorimetry so they can convert before compositing; under `"native"` they advertise the sRGB destination the compositor really writes, so a tagged client's pixels are unchanged by the profile. The advertised curve is applied without substituting a different one: a curve that matches a named transfer function is stated by name, an exact power is stated as that power, and any other curve is stated by the profile's own bytes plus a decode table on the description. A pipeline that can only use named transfer functions still receives one from `get_preferred_parametric`, and a client tagging a buffer with an advertised description gets the encoding that description states; a sampled curve is carried by a 1024-entry table whose unit-suite worst error is 0.002 of an 8-bit code. Characterization applies to SDR composition only; under HDR the PQ/BT.2020 pipeline behaves exactly as described above.

ICC import accepts matrix-shaper profiles whose three color channels share one transfer curve. sRGB and gamma 2.2 map to named curves, linear and other exact powers are stated as that power, and any other curve is sampled into a lookup table, with a non-monotone table first flattened to its monotone envelope and reported. LUT-based profiles and profiles whose channel curves differ are rejected with a warning and fall back down the chain: one curve cannot render three different channels without a cast the profile does not describe. A `vcgt` tag is ignored: it is a pre-dispatch gamma ramp, not content colorimetry.

While a mapping is staged, direct scanout is disabled (`color_profile = "none"` restores it) and SDR frames leave the 8-bit fast path for FP16 composition with a per-damage re-encode. That increases composition and encoding work on redraws and may increase GPU power use. Output screenshots and recordings show the on-screen mapped result. A window capture keeps the client's own colorimetry and metadata instead, so the same window can differ from the screen while a mapping is staged.

Clients may also import their own ICC profiles through the color-management protocol. Only the three exactly renderable curves are accepted; anything else fails that description instead of rendering an approximation. A client that stages or retains more ICC bytes than its budget allows, holds too many ICC creators at once, or passes an unusable file descriptor gets a protocol error and is disconnected rather than a graceful failure. See the [color-management design note](../design/color-management.md) for the request contract and the limits.

Run `umbriel color` to inspect the effective state: the configured `color_profile` and the source that actually characterized the output (`color_profile_resolved`, which reports `edid` after a profile fell back), `sdr_gamut`, whether a mapping is staged (`mapping staged`, the JSON's `sdr_gamut_staged`), and the panel's `panel_primaries` and `panel_transfer`. The JSON form (`umbriel color --json`) reports the same fields plus the active bit depth and fallback reason.

ICC support is built with lcms2; see [PACKAGING.md](../../PACKAGING.md) for the build option and what degrades without it.

## Bit depth

Set `bit_depth = 10` to request a 10-bit SDR compositor render format:

```toml
[output.DP-1]
bit_depth = 10
```

Umbriel selects XR30 (`DRM_FORMAT_XRGB2101010`) or XB30 (`DRM_FORMAT_XBGR2101010`) when the backend accepts it. XB30 is tried first if it is already active. Otherwise, XR30 is tried first. If no 10-bit format commits, the output falls back to 8-bit. HDR uses 10-bit independently of this setting.

While a 10-bit format is active, blur and effects intermediate buffers are upgraded to FP16 precision, provided the renderer supports FP16 render targets and linear filtering of half-float textures. Otherwise, they remain 8-bit.

`bit_depth = 10` controls the compositor render format only. It does not guarantee that the physical display link runs at 10 bits per channel. The number of bits delivered to the panel depends on the display's EDID, cable, and driver. Run `umbriel color` to confirm the active render format that the compositor committed.

In `umbriel color --json`, `bit_depth` is the configured value and `bit_depth_active` reports whether an enabled SDR output is currently using XR30 or XB30. `bit_depth_fallback_reason` explains a failed 10-bit request. It is empty while HDR is active.

### VRR fallback

When VRR is also requested and the output supports adaptive sync, Umbriel tests the formats with VRR first, in the order described above. If neither passes, it tests them without VRR in the same order. Once a format passes its test, Umbriel attempts to commit it. If that commit fails with VRR, it retries the same format without VRR, without another test. A failed commit does not try the other format. If no 10-bit format commits, it falls back to 8-bit. HDR follows the same retry rule before falling back to SDR.

### Direct scanout with 10-bit

Direct scanout remains enabled by the `direct_scanout` setting, but it may be less likely to engage while 10-bit rendering is active. Direct scanout requires the client buffer format to exactly match what KMS accepts for the plane. Set `direct_scanout = false` to disable direct scanout for an output entirely.

### Screencopy and capture

Shared-memory captures through screencopy and image-copy use an 8-bit format even when the output render format is XR30 or XB30. This applies to both 10-bit SDR outputs and the SDR capture view produced while HDR is active, improving compatibility with clients whose shared-memory paths assume 8-bit pixels. DMA-BUF capture may retain the negotiated 10-bit storage format. During HDR, that buffer still contains Umbriel's SDR capture view. Raw export-DMA-BUF is not available.
