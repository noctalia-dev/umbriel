# Appearance

Configure colors, window appearance, blur, shadows, and other visual effects.

## Colors

```toml
[colors]
background = "#141419FF"
text_primary = "#E8E8EAFF"
text_muted = "#8A8A92FF"
accent_primary = "#7AA3FFFF"
accent_secondary = "#F5C96BFF"
warning = "#F5C96BFF"
error = "#FF6B6BFF"
insert_hint = "#7FC8FF80"
backdrop = "#000000FF"
shadow = "#0000007F"
```

Every color Umbriel paints is configured here. The seven semantic palette keys
are the ones Umbriel's own panels use, such as the keybind cheatsheet and the
configuration diagnostics panel. `insert_hint`, `backdrop`, and `shadow` cover
the remaining compositor surfaces, and the [`[colors.border]`](#border-colors)
and [`[colors.overview]`](#overview-colors) sub-tables cover window and overview
chrome. Colors are `#RRGGBB` or `#RRGGBBAA`.

| Key                | Type  | Default     | Description                                        |
| ------------------ | ----- | ----------- | -------------------------------------------------- |
| `background`       | color | `#141419FF` | Shared background for internal panels and banners. |
| `text_primary`     | color | `#E8E8EAFF` | Primary text.                                      |
| `text_muted`       | color | `#8A8A92FF` | Secondary help and status text.                    |
| `accent_primary`   | color | `#7AA3FFFF` | Primary emphasis, including titles, key chords, and the cheatsheet border. |
| `accent_secondary` | color | `#F5C96BFF` | Secondary emphasis, including group headings.      |
| `warning`          | color | `#F5C96BFF` | Warning status text, and the diagnostics panel border when it reports only warnings. |
| `error`            | color | `#FF6B6BFF` | Error text, and the border of the session-quit confirmation and of a diagnostics panel reporting an error. |
| `insert_hint`      | color | `#7FC8FF80` | Drop-target preview during drag.                   |
| `backdrop`         | color | `#000000FF` | Background for fullscreen gaps and the lock screen. |
| `shadow`           | color | `#0000007F` | Window shadow color.                               |

Key chord backgrounds are derived from `background` and `text_primary`; they
remain opaque so text stays legible over translucent panels.

Modal panels use their semantic colors as borders: one logical pixel of
`accent_primary` around the keybind cheatsheet, and two logical pixels around
the smaller session-quit confirmation and the configuration diagnostics panel.

### Border colors

```toml
[colors.border]
focused = "#7AA3FFFF"
unfocused = "#292933FF"
scratchpad_focused = "#E5C07BFF"
scratchpad_unfocused = "#5C4A2AFF"
outer = "#1A1A1FFF"
```

| Key                    | Type  | Default     | Description                                     |
| ---------------------- | ----- | ----------- | ----------------------------------------------- |
| `focused`              | color | `#7AA3FFFF` | Border color for the focused window.            |
| `unfocused`            | color | `#292933FF` | Border color for unfocused windows.             |
| `scratchpad_focused`   | color | `#E5C07BFF` | Border color for the focused scratchpad window. |
| `scratchpad_unfocused` | color | `#5C4A2AFF` | Border color for unfocused scratchpad windows.  |
| `outer`                | color | `#1A1A1FFF` | Outer border color. It has no focus variant.    |

Border widths, the corner radius, and the other decoration geometry live in
[`[appearance]`](#window-appearance).

### Overview colors

```toml
[colors.overview]
background_tint = "#10101430"
workspace_background = "#00000044"
badge = "#7AA3FFFF"
```

| Key                    | Type  | Default     | Description                                                                                    |
| ---------------------- | ----- | ----------- | ---------------------------------------------------------------------------------------------- |
| `background_tint`      | color | `#10101430` | Tint composited over the desktop background. Alpha `00` leaves it untouched; `FF` hides it.    |
| `workspace_background` | color | `#00000044` | Rounded background behind each workspace. Alpha `00` makes it invisible; `FF` makes it opaque. |
| `badge`                | color | `#7AA3FFFF` | Keyboard shortcut badges on overview cards.                                                    |

`overview.workspace_wallpaper` mirrors the output's background- and bottom-layer
surfaces over `workspace_background`, which then only shows on an output where
no client maps one.

The rest of the overview's behavior is configured in
[`[overview]`](workspaces-overview.md#settings-and-behavior).

## Window appearance

```toml
[appearance]
prefer_no_csd = true
border_width = 2               # 0-100
outer_border_width = 0         # 0-100
corner_radius = 10             # 0-100, final outer edge; 0 disables
drag_opacity = 0.75
```

| Key                           | Type  | Default     | Description                                                                                                                                       |
| ----------------------------- | ----- | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `prefer_no_csd`               | bool  | `true`      | Prefer Umbriel's border-only server decoration. Set to `false` to let clients draw their own decorations.                                        |
| `border_width`                | int   | `2`         | Inner border width in logical pixels (0-100), including around rounded corners.                                                                   |
| `outer_border_width`          | int   | `0`         | Ring outside the inner border in logical pixels (0-100).                                                                                          |
| `corner_radius`               | int   | `10`        | Final decorated outer-edge radius in logical pixels (0-100). 0 disables.                                                                          |
| `drag_opacity`                | float | `0.75`      | Opacity of the window while dragging.                                                                                                             |

With `prefer_no_csd = true`, Umbriel advertises the XDG and KDE decoration
managers and prefers its border-only server decoration. An explicit request for
client-side decorations is still honored. With `false`, Umbriel hides both
managers from newly connected clients so toolkits such as Qt draw their own
decorations. Protocol visibility is fixed when an application connects, so
restart applications after changing this setting.

Window decoration colors are configured in [`[colors.border]`](#border-colors),
with the drop-target preview and the fullscreen backdrop in
[`[colors]`](#colors).

Border widths and corner radius are measured in logical pixels. `corner_radius`
describes the final outside edge of the complete decoration. Positive color-seam
and content radii decrease smoothly as their border inset grows, but never
collapse to square; `corner_radius = 0` keeps every contour square. Inner and
outer borders render outside the window and are included in layout spacing.

See [Scratchpads](scratchpad.md) for how scratchpad windows behave and use the
dedicated border colors.

### Blur

```toml
[appearance.blur]
enabled = true
optimized = true
passes = 3        # 0-8
radius = 5        # 0-100
noise = 0.02      # 0.0-1.0
brightness = 0.9  # 0.0-2.0
contrast = 0.9    # 0.0-2.0
saturation = 1.1  # 0.0-2.0
```

`enabled` is the master switch. Individual surfaces must still opt in through
[window rules](window-rules.md) or [layer rules](layer-rules.md).
Blur only renders where a surface is transparent. Sampling remains confined to
the surface's owning output when a window overflows into a neighbouring output.
Disabling the master switch also releases the per-output blur render targets.

| Key          | Type  | Default | Description                                                                                                                       |
| ------------ | ----- | ------- | --------------------------------------------------------------------------------------------------------------------------------- |
| `enabled`    | bool  | `true`  | Master blur switch.                                                                                                              |
| `optimized`  | bool  | `true`  | Use one cached background blur per output for all surfaces. This is the X-ray mode: translucent surfaces blur the background beneath the window stack instead of the surfaces behind them. `false` drops the cache and its two per-output buffers unless a window or layer rule sets `blur_optimized = true`. |
| `passes`     | int   | `3`     | Blur passes (0-8). 0 disables.                                           |
| `radius`     | int   | `5`     | Blur radius (0-100). 0 disables.                                         |
| `noise`      | float | `0.02`  | Noise overlay (0.0-1.0).                                                 |
| `brightness` | float | `0.9`   | Brightness adjustment (0.0-2.0).                                         |
| `contrast`   | float | `0.9`   | Contrast adjustment (0.0-2.0).                                           |
| `saturation` | float | `1.1`   | Saturation adjustment (0.0-2.0).                                         |

### Shadow

```toml
[appearance.shadow]
enabled = true
softness = 10      # 0-200
offset_x = 2       # -200 to 200
offset_y = 2
```

Drop shadow behind windows (tiled and floating). Hidden while fullscreen. The
shadow color is [`colors.shadow`](#colors).

| Key        | Type  | Default     | Description                                                            |
| ---------- | ----- | ----------- | ---------------------------------------------------------------------- |
| `enabled`  | bool  | `true`      | Enable drop shadows.                                                   |
| `softness` | int   | `10`        | Gaussian blur sigma in pixels (0-200). 0 produces a hard-edged shadow. |
| `offset_x` | int   | `2`         | Horizontal shadow offset (-200 to 200).                                |
| `offset_y` | int   | `2`         | Vertical shadow offset (-200 to 200).                                  |
