# Appearance

This page covers colors, window appearance, blur, and shadows.

## Colors

```toml
[colors]
background = "#141419F0"
text_primary = "#E8E8EAFF"
text_muted = "#8A8A92FF"
accent_primary = "#7AA3FFFF"
accent_secondary = "#F5C96BFF"
warning = "#F5C96BFF"
error = "#FF6B6BFF"
```

Shared semantic colors for Umbriel-owned interface surfaces such as the keybind
cheatsheet and configuration diagnostic banner. Colors are `#RRGGBB` or
`#RRGGBBAA`.

| Key                | Type  | Default     | Description                                        |
| ------------------ | ----- | ----------- | -------------------------------------------------- |
| `background`       | color | `#141419F0` | Shared background for internal panels and banners. |
| `text_primary`     | color | `#E8E8EAFF` | Primary text.                                      |
| `text_muted`       | color | `#8A8A92FF` | Secondary help and status text.                    |
| `accent_primary`   | color | `#7AA3FFFF` | Primary emphasis, including titles and key chords. |
| `accent_secondary` | color | `#F5C96BFF` | Secondary emphasis, including group headings.      |
| `warning`          | color | `#F5C96BFF` | Warning status text.                               |
| `error`            | color | `#FF6B6BFF` | Error status text.                                 |

Key chord backgrounds are derived from `background` and `text_primary`; they
remain opaque so text stays legible over translucent panels.

## Window appearance

```toml
[appearance]
prefer_no_csd = true
border_width = 2               # 0-100
outer_border_width = 0         # 0-100
corner_radius = 10             # 0-100, 0 disables
border_focused = "#7AA3FFFF"   # #RRGGBB or #RRGGBBAA
border_unfocused = "#292933FF"
scratchpad_border_focused = "#E5C07BFF"
scratchpad_border_unfocused = "#5C4A2AFF"
outer_border_color = "#1A1A1FFF"
insert_hint_color = "#7FC8FF80"
backdrop_color = "#000000FF"
animation_ms = 200             # 1-10000
drag_opacity = 0.75
```

| Key                           | Type  | Default     | Description                                                                                                                                       |
| ----------------------------- | ----- | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `prefer_no_csd`               | bool  | `true`      | Ask clients to omit client-side decorations (xdg-decoration). Clients that explicitly request CSD are still honored. Restart apps after changing. |
| `border_width`                | int   | `2`         | Inner border width in logical pixels (0-100), including around rounded corners.                                                                   |
| `outer_border_width`          | int   | `0`         | Ring outside the inner border in logical pixels (0-100).                                                                                          |
| `corner_radius`               | int   | `10`        | Rounded corner radius (0-100). 0 disables.                                                                                                        |
| `border_focused`              | color | `#7AA3FFFF` | Border color for the focused window.                                                                                                              |
| `border_unfocused`            | color | `#292933FF` | Border color for unfocused windows.                                                                                                               |
| `scratchpad_border_focused`   | color | `#E5C07BFF` | Border color for the focused scratchpad window.                                                                                                   |
| `scratchpad_border_unfocused` | color | `#5C4A2AFF` | Border color for unfocused scratchpad windows.                                                                                                    |
| `outer_border_color`          | color | `#1A1A1FFF` | Outer border color (no focus variant).                                                                                                            |
| `insert_hint_color`           | color | `#7FC8FF80` | Drop-target preview during drag.                                                                                                                  |
| `backdrop_color`              | color | `#000000FF` | Background for fullscreen gaps and lock screen.                                                                                                   |
| `animation_ms`                | int   | `200`       | Animation duration in milliseconds (1-10000).                                                                                                     |
| `drag_opacity`                | float | `0.75`      | Opacity of the window while dragging.                                                                                                             |

Colors are `#RRGGBB` or `#RRGGBBAA`.

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
[window rules](rules.md) or [layer rules](rules.md#layer-rules).
Blur only renders where a surface is transparent. Sampling remains confined to
the surface's owning output when a window overflows into a neighbouring output.
Disabling the master switch also releases SceneFX's per-output blur render
targets.

| Key          | Type  | Default | Description                                                              |
| ------------ | ----- | ------- | ------------------------------------------------------------------------ |
| `enabled`    | bool  | `true`  | Master blur switch.                                                      |
| `optimized`  | bool  | `true`  | Cache one background blur per output instead of recomputing per surface. |
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
color = "#0000007F"
```

Drop shadow behind windows (tiled and floating). Hidden while fullscreen.

| Key        | Type  | Default     | Description                                                            |
| ---------- | ----- | ----------- | ---------------------------------------------------------------------- |
| `enabled`  | bool  | `true`      | Enable drop shadows.                                                   |
| `softness` | int   | `10`        | Gaussian blur sigma in pixels (0-200). 0 produces a hard-edged shadow. |
| `offset_x` | int   | `2`         | Horizontal shadow offset (-200 to 200).                                |
| `offset_y` | int   | `2`         | Vertical shadow offset (-200 to 200).                                  |
| `color`    | color | `#0000007F` | Shadow color.                                                          |
