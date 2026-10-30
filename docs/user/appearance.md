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

`focused` and `unfocused` color the inner border; `outer` colors the optional outer border. A [window rule](window-rules.md#border-colors) can override any of them for the windows it matches.

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

### Tab bar colors

```toml
[colors.tab_bar]
background = "#1A1A1FFF"
text = "#8A8A92FF"
active = "#7AA3FFFF"
active_text = "#141419FF"
active_unfocused = "#292933FF"
active_unfocused_text = "#E8E8EAFF"
urgent = "#FF6B6BFF"
urgent_text = "#141419FF"
```

| Key                     | Description                                       |
| ----------------------- | ------------------------------------------------- |
| `background`            | Slots of tabs that are neither active nor urgent. |
| `text`                  | Their titles.                                     |
| `active`                | The active tab's slot in the focused column.      |
| `active_text`           | Its title.                                        |
| `active_unfocused`      | The active tab's slot in any other column.        |
| `active_unfocused_text` | Its title.                                        |
| `urgent`                | The slot of a hidden tab that asks for attention. |
| `urgent_text`           | Its title.                                        |

## Window appearance

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

| Key                  | Default | Description                                                                                     |
| -------------------- | ------- | ----------------------------------------------------------------------------------------------- |
| `prefer_no_csd`      | `true`  | Advertise a preference for server decorations. The optional title bar is configured separately. |
| `border_width`       | `2`     | Inner border width in logical pixels.                                                           |
| `outer_border_width` | `0`     | Outer ring width in logical pixels.                                                             |
| `corner_radius`      | `10`    | Radius of the complete decorated window.                                                        |
| `drag_opacity`       | `0.75`  | Opacity while dragging a window.                                                                |
| `opaque_fullscreen`  | `true`  | Draw fullscreen windows over the backdrop and ignore window rule `opacity`.                     |
| `downsample`         | `"box"` | How the scene samples content that shrinks: `"box"`, `"bilinear"`, or `"nearest"`.              |

With `opaque_fullscreen = false`, a fullscreen window that is translucent shows the desktop behind it instead of the backdrop, and can be blurred. A window is translucent when its rule opacity is below 1 or the application itself draws transparent content. Other fullscreen windows stay opaque and skip blur.

X11 bounding-shape cutouts show what is behind the window with either `opaque_fullscreen` setting.

`downsample` picks the sampling method whenever content shrinks: the [render_scale supersample](outputs.md#supersampling-with-render_scale) downsample, a window scaled down while it animates, or a thumbnail. `box` averages every source texel the destination pixel covers, which keeps text and fine detail crisp after supersampling; `bilinear` and `nearest` fetch a single texel per pixel and alias more. A reload applies the method immediately.

Set `prefer_no_csd = false` to let newly connected applications draw their own decorations. Restart applications after changing it because decoration protocol availability is fixed when an application connects.

Borders render outside window content and are included in layout spacing. `corner_radius = 0` keeps every contour square. A [window rule](window-rules.md#decoration) can override `border_width`, `outer_border_width`, and `corner_radius` for the windows it matches.

### Title bar

Enable a simple server-side title bar on decorated windows in every layout:

```toml
[appearance.title_bar]
mode = "off"
height = 28
font = "sans 10"
button_position = "right"
buttons = ["minimize", "maximize", "close"]
button_corner_radius = -1

[colors.title_bar]
background = "#292933FF"
background_unfocused = "#1A1A1FFF"
text = "#E8E8EAFF"
text_unfocused = "#8A8A92FF"
close = "#FF6B6BFF"
maximize = "#7AA3FFFF"
minimize = "#FFD166FF"
```

Choose a title bar mode:

| Mode           | Behavior                                                                                   |
| -------------- | ------------------------------------------------------------------------------------------ |
| `"off"`        | Hide Umbriel's title bar (the default).                                                    |
| `"negotiated"` | Show for Wayland windows only when decoration negotiation selects server-side decorations. |
| `"on"`         | Follow client decoration preferences; show when no preference is known.                    |
| `"forced"`     | Show regardless of client preference.                                                      |

`height` is 16 to 128 logical pixels, and `font` is a Pango font description. `button_position` accepts `"left"` or `"right"`. `buttons` lists the buttons to show, from the title outward: the last one sits at the bar's edge, so the default puts close outermost on either side. Leave a name out to hide that button, or use `[]` for a bar without buttons; each of `"minimize"`, `"maximize"`, and `"close"` may appear once. The buttons are colored and have no icons. `button_corner_radius` uses logical pixels, from `0` to `100`. Its default, `-1`, follows the window's `corner_radius`, including window rules. Use `0` for square buttons or a small positive value for rounded squares. The radius is capped at half the button size, producing circles at larger values. The shape scales with the output and overview zoom, and updates on reload. The maximize button is disabled for fixed-size windows. The minimize button shows only while the window can [minimize](layout.md#minimize), so tiled and pinned windows do not have one. `background` and `text` apply to the focused window; their `_unfocused` variants apply to other windows. Colors and title bar settings reload live.

The title bar follows its window's `blur`, `blur_optimized`, and `blur_ignore_alpha` rules, together with the global [blur settings](#blur). Give `background` and `background_unfocused` some transparency, such as `"#292933CC"`, to see the blurred backdrop. The alpha threshold applies to the title bar's own background color, independently of the application's content. Opaque title bar backgrounds remain opaque with blur enabled, including after leaving overview or switching focus between windows. No layer rule is needed. Dragging, overview previews, and close animations keep the blur with the window.

Rounded windows keep a straight join between the title bar and transparent content, including the content's blur and overview preview. The outer frame retains its rounded corners; hiding the title bar restores rounding to the top of the content.

In `"on"` mode, Wayland windows that request server decorations, or have no known decoration preference, receive a bar. Explicit client-side decoration requests suppress it. Both xdg-decoration and the older server-decoration protocol are tracked; xdg-decoration takes priority when a window uses both. `"negotiated"` follows the same preferences but hides the bar when neither protocol is in use. A client that creates an xdg-decoration object and accepts the compositor's default still negotiates server-side decorations and receives a bar. This mode avoids adding bars to Wayland apps that draw their own headerbars or intentionally create frameless windows without negotiating decorations. Managed Xwayland windows follow their decoration hints in both `"on"` and `"negotiated"` modes. Popups and unmanaged Xwayland windows do not receive a title bar.

A [window rule](window-rules.md#decoration) can set `title_bar` to `"on"`, `"negotiated"`, `"forced"`, or `"off"`. `"forced"` adds a bar even when the application requests its own decorations; `"off"` hides it; `"on"` restores automatic selection after an earlier matching override. An omitted rule inherits the global mode. An explicit rule overrides it, so `"forced"` can enable a bar with global `"off"`, and `"on"` can honor client preferences with global `"forced"`. A `"negotiated"` rule requires negotiation for the matching Wayland windows, regardless of the global mode. Existing windows update when their preference or matching rule changes, or when the setting reloads.

A window rule's `title_bar_buttons` replaces `buttons` for the windows it matches, for example to keep only close on dialogs:

```toml
[[window_rule]]
match.is_dialog = true
title_bar_buttons = ["close"]
```

Application-drawn headerbars, tab strips, and custom controls remain part of the application content. To avoid duplicate title bars, select the application's native or system title bar option where available. `prefer_no_csd = true` still advertises Umbriel's preference to applications that support decoration negotiation.

Drag empty title bar space with the left button to move a window without a modifier, or double-click it to toggle maximize. Click a colored button to maximize/restore or close the window. Buttons activate on release inside the same button. Floating windows also have modifier-free resize grips around their borders and corners, including when the Umbriel title bar is hidden. This also applies to individually floated windows in tiling layouts. Application content keeps its normal pointer behavior.

Enabled buttons show a pointing cursor on hover. Their colors lighten against dark title bars and darken against light ones; pressing makes them darker again. Disabled maximize buttons keep the arrow cursor and do not highlight.

Tab group members show the tab bar instead. Fullscreen hides the title bar. Normal maximize and maximize-to-edges keep it, so its maximize button or a double-click can restore the window. A maximized-to-edges title bar has square corners and occupies the top of the usable area, above the application content; borders and shadows stay hidden.

The bar occupies space inside a tile's allocation; configure sizes, IPC window coordinates, and window captures continue to describe application content. Borders and shadows normally wrap the title bar and content together. To hide the bar only while maximized to edges, use a [window rule](window-rules.md#decoration) with `match.is_maximized_to_edges = true` and `title_bar = "off"`.

### Tab bar

A [tab group](layout.md#tab-groups) draws a bar along one edge, with a slot for each tab. Its `‹` and `›` buttons become `▴` and `▾` on a side bar.

```toml
[appearance.tab_bar]
style = "titles"
position = "top"
height = 24
font = "sans 10"
padding = 8
tab_gap = 2
gap = -1
corner_radius = -1
title_format = "{title}"
title_align = "center"
visible = true
hide_when_single = false
max_tabs = 0
overflow = "scroll"
```

| Key                | Default     | Description                                                                                                                                                                                                                                                                     |
| ------------------ | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `style`            | `"titles"`  | `"titles"` shows each tab's title in its slot. `"indicator"` draws the slots alone; pair it with a small `height` for a thin strip.                                                                                                                                             |
| `position`         | `"top"`     | The group edge the bar sits on: `"top"`, `"bottom"`, `"left"`, or `"right"`. On the top or bottom the slots sit side by side; on the left or right they stack, a vertical tab list whose titles still read across.                                                              |
| `height`           | `24`        | Thickness in logical pixels, from 2 to 400: the bar's height on the top or bottom, its width on the left or right. The group reserves the bar and its `gap` beside it. Give a side bar with titles room for them, such as `160`.                                                |
| `font`             | `"sans 10"` | Title font as a Pango description, such as `"Inter Bold 9"`.                                                                                                                                                                                                                    |
| `padding`          | `8`         | Space a title keeps from either end of its slot.                                                                                                                                                                                                                                |
| `tab_gap`          | `2`         | Space between neighbouring slots.                                                                                                                                                                                                                                               |
| `gap`              | `-1`        | Space between the bar and its window, from 0 to 500; `-1` follows the [layout `gap`](layout.md#shared-settings).                                                                                                                                                                |
| `corner_radius`    | `-1`        | Slot corner radius; `-1` follows `corner_radius`.                                                                                                                                                                                                                               |
| `title_format`     | `"{title}"` | Slot label. `{title}`, `{app_id}`, and `{index}` (from 1) are replaced; a window without a title shows its app id.                                                                                                                                                              |
| `title_align`      | `"center"`  | `"left"`, `"center"`, or `"right"` within the slot.                                                                                                                                                                                                                             |
| `visible`          | `true`      | Draw bars. `false` hides the bar of every group that has not shown its own with `column-show-tab-bar` or `column-toggle-tab-bar`; the tabs take its space and keep working from the keyboard. Those actions hide or show one group's bar whatever this says.                    |
| `hide_when_single` | `false`     | Draw no bar, and reserve no space, while a tab group holds one window. It stays a group, so the next window to join becomes a tab.                                                                                                                                              |
| `max_tabs`         | `0`         | Slots the bar shows at once, from 0 to 64; `0` shows every tab.                                                                                                                                                                                                                 |
| `overflow`         | `"scroll"`  | What a bar does with more tabs than `max_tabs`. `"scroll"` keeps the slots at their size and shows a run of tabs that follows the active one, with a `‹` and a `›` button at the ends that select the previous and next tab. `"shrink"` narrows the slots until every tab fits. |

The bar is hidden while its window is fullscreen or maximized to the edges. Titles are drawn at the output's scale and cached, so switching tabs never renders them again. A scrolled bar draws and renders titles only for the tabs it shows, however many the group holds. Clicking `‹` or `›` cycles through every tab, wrapping from the last to the first and back, and the strip follows. The wheel over the bar, the tab actions, and focus move the active tab too, and the strip follows it one slot at a time.

### Blur

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

`enabled` is the master switch. Individual surfaces still opt in through [window rules](window-rules.md) or [layer rules](layer-rules.md). Blur appears only where a surface is transparent.

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

Optimized blur samples the background beneath the window stack. Set it to `false` when translucent surfaces should blur the surfaces directly behind them, at a higher rendering cost. The shared background cache follows background layer-shell updates, including animated map and unmap transitions. Unchanged commits that only request frame callbacks reuse the cached blur.

### Shadow

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

A window's shadow falls on everything below it, including other floating, pinned, or scratchpad windows it overlaps. Tiled windows never shadow each other. Shadows are hidden for fullscreen windows. During a [custom window animation](animation.md#custom-effects), the shadow follows the visible shape produced by the shader. A [window rule](window-rules.md#decoration) can turn the shadow on or off for the windows it matches.
