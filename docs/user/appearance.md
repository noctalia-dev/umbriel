# Appearance

Configure Umbriel's colors, window decorations, blur, and shadows.

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

| Key | Description |
| --- | --- |
| `background` | Background for Umbriel panels and banners. |
| `text_primary` | Primary text. |
| `text_muted` | Secondary help and status text. |
| `accent_primary` | Titles, key chords, and primary emphasis. |
| `accent_secondary` | Secondary emphasis and group headings. |
| `warning` | Warning text and borders. |
| `error` | Error text and confirmation borders. |
| `insert_hint` | Drop-target preview during dragging. |
| `backdrop` | Fullscreen background and RGB color of the opaque emergency lock blank. |
| `shadow` | Window shadow color. |

### Border colors

```toml
[colors.border]
focused = "#7AA3FFFF"
unfocused = "#292933FF"
scratchpad_focused = "#E5C07BFF"
scratchpad_unfocused = "#5C4A2AFF"
outer = "#1A1A1FFF"
```

The first four values select focused and unfocused colors for regular and
scratchpad windows. `outer` colors the optional outer border.

### Overview colors

```toml
[colors.overview]
background_tint = "#10101430"
workspace_background = "#00000044"
badge = "#7AA3FFFF"
```

| Key | Description |
| --- | --- |
| `background_tint` | Tint over the desktop behind the overview. |
| `workspace_background` | Background behind each workspace preview. |
| `badge` | Shortcut badge color. |

See [Workspaces Overview](workspaces-overview.md#settings-and-behavior) for
overview behavior.

## Window appearance

```toml
[appearance]
prefer_no_csd = true
border_width = 2
outer_border_width = 0
corner_radius = 10
drag_opacity = 0.75
opaque_fullscreen = true
```

| Key | Default | Description |
| --- | --- | --- |
| `prefer_no_csd` | `true` | Prefer Umbriel's border-only server decoration. |
| `border_width` | `2` | Inner border width in logical pixels. |
| `outer_border_width` | `0` | Outer ring width in logical pixels. |
| `corner_radius` | `10` | Radius of the complete decorated window. |
| `drag_opacity` | `0.75` | Opacity while dragging a window. |
| `opaque_fullscreen` | `true` | Draw fullscreen windows over the backdrop and ignore window rule `opacity`. |

With `opaque_fullscreen = false`, a fullscreen window that is translucent shows
the desktop behind it instead of the backdrop, and can be blurred. A window is
translucent when its rule opacity is below 1 or the application itself draws
transparent content. Other fullscreen windows stay opaque and skip blur.

Set `prefer_no_csd = false` to let newly connected applications draw their own
decorations. Restart applications after changing it because decoration protocol
availability is fixed when an application connects.

Borders render outside window content and are included in layout spacing.
`corner_radius = 0` keeps every contour square.

### Blur

```toml
[appearance.blur]
enabled = true
optimized = true
passes = 3
radius = 5
noise = 0.02
brightness = 0.9
contrast = 0.9
saturation = 1.1
```

`enabled` is the master switch. Individual surfaces still opt in through
[window rules](window-rules.md) or [layer rules](layer-rules.md). Blur appears
only where a surface is transparent.

| Key | Default | Description |
| --- | --- | --- |
| `enabled` | `true` | Enable blur rendering. |
| `optimized` | `true` | Share one cached background blur across surfaces on an output. |
| `passes` | `3` | Blur passes from 0 to 8. |
| `radius` | `5` | Blur radius from 0 to 100. |
| `noise` | `0.02` | Noise overlay from 0.0 to 1.0. |
| `brightness` | `0.9` | Brightness multiplier from 0.0 to 2.0. |
| `contrast` | `0.9` | Contrast multiplier from 0.0 to 2.0. |
| `saturation` | `1.1` | Saturation multiplier from 0.0 to 2.0. |

Optimized blur samples the background beneath the window stack. Set it to
`false` when translucent surfaces should blur the surfaces directly behind
them, at a higher rendering cost.

### Shadow

```toml
[appearance.shadow]
enabled = true
softness = 10
offset_x = 2
offset_y = 2
```

| Key | Default | Description |
| --- | --- | --- |
| `enabled` | `true` | Draw shadows behind tiled and floating windows. |
| `softness` | `10` | Blur softness from 0 to 200. |
| `offset_x` | `2` | Horizontal offset from -200 to 200. |
| `offset_y` | `2` | Vertical offset from -200 to 200. |

A window's shadow falls on everything below it, including other floating,
pinned, or scratchpad windows it overlaps. Tiled windows never shadow each
other. Shadows are hidden for fullscreen windows. During a
[custom window animation](animation.md#custom-glsl-shaders), the shadow follows
the visible shape produced by the shader.
