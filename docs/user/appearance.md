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


## Nine-rect textured frames

Nine-rect frames replace the procedural border bands and corner radius with a
static PNG. Enable them with:

```toml
[appearance]
use_nine_rect = true

[appearance.nine_rect]
texture = "themes/frame.png"
slice_px = { top = 16, bottom = 16, left = 16, right = 16 }
horizontal_stretch_mode = "stretch"
vertical_stretch_mode = "tile"
center_mode = "none"
```

A complete sample, including PNG artwork, lives in
[`examples/nine-rect`](../../examples/nine-rect/theme.toml). Include its `theme.toml`
from your configuration. Asset paths resolve relative to the file that defines
them, including included theme files.

Choose exactly one slice table. `slice_px` takes nonnegative integer source
pixels. Alternatively, `slice = { top = 0.25, bottom = 0.25, left = 0.25, right = 0.25 }`
takes fractions of the source height for top/bottom and width for left/right.
Fractions round to the nearest source pixel, with halves rounded upward. Both
original and rounded sums must fit their source dimensions. All four sides
are required. Empty source regions draw nothing.

Corners keep their source size in logical pixels. Each axis accepts `stretch`
(the default) or `tile`. Tile repetitions keep their source period and crop
the last repetition. Output scale still applies normally; there is no separate
artwork scale setting. Visual sampling uses nearest filtering.

By default the destination center is the client rectangle. If the artwork has
transparent padding inside its slice bands, set separate content insets so the
client fills that padding without changing the corners or tile period:

```toml
slice_px = { top = 16, bottom = 16, left = 16, right = 16 }
content_inset_px = { top = 12, bottom = 12, left = 12, right = 12 }
```

`content_inset_px` measures from the outer artwork edge to the client edge in
logical pixels. Each value must be an integer between zero and its corresponding
slice size. It defaults to the slice sizes. Here the four inner pixels of each
slice band overlap the client; transparent artwork reveals the client beneath.
Alternatively, `content_inset` takes fractions of the source image height for
top/bottom and width for left/right, with the same half-up rounding as `slice`:

```toml
# For a 64×64 texture, this is equivalent to 12 pixels on each side.
content_inset = { top = 0.1875, bottom = 0.1875, left = 0.1875, right = 0.1875 }
```

Use only one of `content_inset` and `content_inset_px`. All four sides are required;
fractions must be between zero and one, and resolved insets must fit their slices.
Either form works with either slice form. Omitting both uses the slice sizes.
Very small clients compress corners to avoid overlap.

`center_mode = "none"` leaves
artwork out of the nine-slice center; `"overlay"` composites it above the application
using its alpha. Clients retain square corners in this mode: transparent corner
artwork does not clip an opaque client into a rounded shape. Focus colors only alter the artwork when theme tint is enabled;
procedural border widths/radius do not alter it. Existing opacity,
shadow, and blur settings still apply, and fullscreen/maximized-to-edge windows
retain their existing decoration visibility policy.

### Optional theme tint

```toml
[appearance.nine_rect]
tint_with_border_color = true # Default: false
```

Tint multiplies the artwork by the existing focused/unfocused border color,
including scratchpad colors and animated focus transitions. White pixels take
the theme color, gray pixels retain their shading, and black stays black.
Colored artwork multiplies with the tint rather than being replaced. Texture
alpha, border-color alpha, and window opacity multiply together.

The whole texture uses one tint, including center artwork when enabled;
`colors.border.outer` does not supply a second tint. Overview cards and closing
snapshots retain their corresponding colors. Change theme colors and reload
normally; no new PNG is needed.
With the toggle off, artwork keeps its original colors.

### Frame geometry and blur

Artwork must be RGB8 or RGBA8 PNG, limited to 4096 pixels per axis and 64 MiB
per encoded file. One texture supplies the entire decoration; no mask is used.

The client rectangle plus its content insets defines the rectangular frame
used by layout, gaps, output alignment, and generated shadows. The frame bands
outside the client accept resize input regardless of artwork transparency.
Artwork overlapping the client passes input through, including center overlays.
Tiled resizing still follows the layout's boundary restrictions, and modifier
move/resize bindings keep their usual behavior.

Blur applies only within the client area, using existing window rules and blur
settings. Transparent decoration outside the client reveals the unblurred
background.

### Reloads

PNGs decode when the theme loads, not during rendering or resizing. Reloading
configuration rereads asset contents, including edits at the same path. Use
`umbriel msg config-reload` after changing only a PNG. Invalid assets or slice
settings reject the reload and leave the previous configuration active. An
identical theme reload is inert; close snapshots retain their captured artwork.

This implementation supports static textures. Animation, SVG, arbitrary
per-piece ordering, and separate nine-rect opacity/shadow/blur settings are not
provided.
