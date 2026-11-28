# Appearance

Configure Umbriel's colors, borders, blur, and shadows. Title bars have their own page, [Title bars](title-bars.md); the tab group bar is under [Tab bar](tab-groups.md#tab-bar); per-application overrides are [Appearance rules](window-rules-appearance.md).

```toml
# Thicker borders and rounder corners
[appearance]
border_width = 3
corner_radius = 14
```

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

Colors use `#RRGGBB` or `#RRGGBBAA`.

| Key                | Description                                                             |
| ------------------ | ----------------------------------------------------------------------- |
| `background`       | Background for Umbriel panels and banners.                              |
| `text_primary`     | Primary text.                                                           |
| `text_muted`       | Secondary help and status text.                                         |
| `accent_primary`   | Titles, key chords, and primary emphasis.                               |
| `accent_secondary` | Secondary emphasis and group headings.                                  |
| `warning`          | Warning text and borders.                                               |
| `error`            | Error text and confirmation borders.                                    |
| `insert_hint`      | Drop-target preview during dragging.                                    |
| `backdrop`         | Fullscreen background and RGB color of the opaque emergency lock blank. |
| `shadow`           | Window shadow color.                                                    |

### Border colors

```toml
[colors.border]
focused = "#7AA3FFFF"
unfocused = "#292933FF"
outer = "#1A1A1FFF"
```

`focused` and `unfocused` color the inner border; `outer` colors the optional outer border. A [window rule](window-rules-appearance.md#borders-that-show-window-state) can override any of them for the windows it matches.

### Overview colors

```toml
[colors.overview]
background_tint = "#10101430"
workspace_background = "#00000044"
badge = "#7AA3FFFF"
```

| Key                    | Description                                |
| ---------------------- | ------------------------------------------ |
| `background_tint`      | Tint over the desktop behind the overview. |
| `workspace_background` | Background behind each workspace preview.  |
| `badge`                | Shortcut badge color.                      |

See [Workspaces Overview](workspaces-overview.md#settings-and-behavior) for overview behavior.

## Windows

```toml
[appearance]
prefer_no_csd = true
border_width = 2
outer_border_width = 0
corner_radius = 10
drag_opacity = 0.75
opaque_fullscreen = true
downsample = "box"
```

| Key                  | Default | Description                                                                                         |
| -------------------- | ------- | --------------------------------------------------------------------------------------------------- |
| `prefer_no_csd`      | `true`  | Ask applications to leave decorations to Umbriel. The [title bar](title-bars.md) is set separately. |
| `border_width`       | `2`     | Inner border width in logical pixels.                                                               |
| `outer_border_width` | `0`     | Outer ring width in logical pixels.                                                                 |
| `corner_radius`      | `10`    | Radius of the whole decorated window; `0` keeps it square.                                          |
| `drag_opacity`       | `0.75`  | Opacity while dragging a window.                                                                    |
| `opaque_fullscreen`  | `true`  | Draw fullscreen windows over the backdrop and ignore window rule `opacity`.                         |
| `downsample`         | `"box"` | Sampling method for content that shrinks. Supports `"box"`, `"bilinear"`, and `"nearest"`.          |

- Borders draw outside the window content and count in layout spacing.
- A [window rule](window-rules-appearance.md#borders-corners-and-shadow) can override the border widths and corner radius for matching windows.
- With `opaque_fullscreen = false`, a translucent fullscreen window shows the desktop behind it and can be blurred.

> [!NOTE]
> Restart applications after changing `prefer_no_csd`; they read it when they connect.

`downsample` picks the sampling method whenever content shrinks: the [render_scale supersample](outputs.md#supersampling-with-render_scale) downsample, a window scaled down while it animates, or a thumbnail. `box` averages each destination pixel's source footprint in linear light. Up to 4:1 on an axis every texel that axis covers counts by its area, at any ratio; beyond that the axis falls back to a fixed grid of bilinear taps, so every texel is weighed up to 8:1 and above that some texels fall between taps. `bilinear` takes one interpolated sample per pixel and `nearest` the closest texel, so both alias more. A reload applies the method immediately, including to a capture already in progress.

## Blur

```toml
[appearance.blur]
enabled = true
optimized = true
passes = 2
radius = 5
noise = 0.02
brightness = 0.9
contrast = 0.9
saturation = 1.1
```

| Key          | Default | Description                                                    |
| ------------ | ------- | -------------------------------------------------------------- |
| `enabled`    | `true`  | Enable blur rendering.                                         |
| `optimized`  | `true`  | Share one cached background blur across surfaces on an output. |
| `passes`     | `2`     | Blur passes from 0 to 8.                                       |
| `radius`     | `5`     | Blur radius from 0 to 100.                                     |
| `noise`      | `0.02`  | Noise overlay from 0.0 to 1.0.                                 |
| `brightness` | `0.9`   | Brightness multiplier from 0.0 to 2.0.                         |
| `contrast`   | `0.9`   | Contrast multiplier from 0.0 to 2.0.                           |
| `saturation` | `1.1`   | Saturation multiplier from 0.0 to 2.0.                         |

- `enabled` is the master switch. Each window or panel still opts in through a [window rule](window-rules-appearance.md) or [layer rule](layer-rules.md), and blur shows only where it is transparent.
- Optimized blur samples the wallpaper. Set `optimized = false` to blur the live windows behind a surface, at a higher rendering cost.
- Apps that clear their content before closing can leave a blurred rectangle; see [Transparent content and blur](window-rules-appearance.md#transparent-content-and-blur).

## Shadow

```toml
[appearance.shadow]
enabled = true
softness = 10
offset_x = 2
offset_y = 2
```

| Key        | Default | Description                                     |
| ---------- | ------- | ----------------------------------------------- |
| `enabled`  | `true`  | Draw shadows behind tiled and floating windows. |
| `softness` | `10`    | Blur softness from 0 to 200.                    |
| `offset_x` | `2`     | Horizontal offset from -200 to 200.             |
| `offset_y` | `2`     | Vertical offset from -200 to 200.               |

- A window's shadow falls on overlapping floating, pinned, and scratchpad windows below it. Tiled windows never shadow each other.
- Fullscreen windows have no shadow.
- A [window rule](window-rules-appearance.md#borders-corners-and-shadow) can turn the shadow on or off for matching windows.
