# Title bars

Umbriel can draw a simple title bar with minimize, maximize, and close buttons on windows in every layout.

```toml
# Show title bars unless the app draws its own
[appearance.title_bar]
mode = "on"
```

To choose per application or per layout, use the `title_bar` key in [Appearance rules](window-rules-appearance.md#title-bars).

## Modes

| Mode           | Behavior                                                                |
| -------------- | ----------------------------------------------------------------------- |
| `"off"`        | No title bar (the default).                                             |
| `"negotiated"` | Show only when the app asks for server-side decorations.                |
| `"on"`         | Follow the app's decoration request; show when it states no preference. |
| `"forced"`     | Always show, whatever the app asks.                                     |

`"negotiated"` leaves apps that draw their own headerbar, or open frameless windows on purpose, without a bar. X11 windows follow their decoration hints in both `"on"` and `"negotiated"`. Popups never get a bar.

## Settings

```toml
[appearance.title_bar]
mode = "off"
height = 28
font = "sans 10"
title_align = "left"
button_position = "right"
buttons = ["minimize", "maximize", "close"]
button_size = -1
button_gap = 8
button_padding = -1
button_corner_radius = -1
button_glyph = "hover"
button_glyph_size = -1
button_glyph_stroke = -1
button_background = true
color_source = "fixed"
color_source_height = 1
title_shadow = true
title_contrast = true
title_contrast_sample = false
```

| Key                     | Default                             | Description                                                                                                                   |
| ----------------------- | ----------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| `mode`                  | `"off"`                             | See [Modes](#modes).                                                                                                          |
| `height`                | `28`                                | Bar height in logical pixels, 16 to 128.                                                                                      |
| `font`                  | `"sans 10"`                         | Pango font description.                                                                                                       |
| `title_align`           | `"left"`                            | `"left"`, `"center"`, or `"right"`. Centered titles move aside only as far as needed to clear the buttons.                    |
| `color_source`          | `"fixed"`                           | Where the bar's color comes from: the palette, the window's own edge, or that edge's mean. See [Color source](#color-source). |
| `color_source_height`   | `1`                                 | Rows of the window's edge an adaptive bar takes, 1 to 4.                                                                      |
| `title_shadow`          | `true`                              | Dark edge under the title and around the symbols, in every `color_source`. See [Contrast](#contrast).                         |
| `title_contrast`        | `true`                              | Reverse the title, and a symbol on the bar or its button color, when it would vanish. See [Contrast](#contrast).              |
| `title_contrast_sample` | `false`                             | Let `"window"` read its edge back so `title_contrast` can judge it. See [Contrast](#contrast).                                |
| `button_position`       | `"right"`                           | `"left"` or `"right"`.                                                                                                        |
| `buttons`               | `["minimize", "maximize", "close"]` | Buttons from the title outward; the last sits at the edge. Each may appear once; `[]` for none.                               |
| `button_size`           | `-1`                                | Button size in logical pixels, 1 to `height`; `-1` derives it from `height`.                                                  |
| `button_gap`            | `8`                                 | Space between buttons, 0 to 100.                                                                                              |
| `button_padding`        | `-1`                                | Space between the buttons and the bar's edge, 0 to 100; `-1` matches the space above the buttons.                             |
| `button_corner_radius`  | `-1`                                | Button corner radius, 0 to 100; `-1` follows the window's `corner_radius`. Large values make circles.                         |
| `button_glyph`          | `"hover"`                           | When to draw the button symbols: `"always"`, `"hover"` (while hovered or pressed), or `"never"`.                              |
| `button_glyph_size`     | `-1`                                | Symbol size in logical pixels, 1 to 128; `-1` is half the button.                                                             |
| `button_glyph_stroke`   | `-1`                                | Symbol line width, 0.5 to 16; `-1` is a twelfth of the button.                                                                |
| `button_background`     | `true`                              | Draw colored button backgrounds; `false` leaves only the symbols.                                                             |

Changes apply to open windows on reload.

## Colors

```toml
[colors.title_bar]
background = "#292933FF"
background_unfocused = "#1A1A1FFF"
text = "#E8E8EAFF"
text_unfocused = "#8A8A92FF"
close = "#FF6B6BFF"
close_glyph = "#141419FF"
close_glyph_hover = "#141419FF"
maximize = "#7AA3FFFF"
maximize_glyph = "#141419FF"
maximize_glyph_hover = "#141419FF"
minimize = "#FFD166FF"
minimize_glyph = "#141419FF"
minimize_glyph_hover = "#141419FF"
```

| Key                                                                 | Description                                      |
| ------------------------------------------------------------------- | ------------------------------------------------ |
| `background`, `background_unfocused`                                | Bar background of the focused and other windows. |
| `text`, `text_unfocused`                                            | Title text of the focused and other windows.     |
| `close`, `maximize`, `minimize`                                     | Button backgrounds.                              |
| `close_glyph`, `maximize_glyph`, `minimize_glyph`                   | Button symbols at rest.                          |
| `close_glyph_hover`, `maximize_glyph_hover`, `minimize_glyph_hover` | Button symbols while hovered or pressed.         |

Symbol colors are used as given, except where `title_contrast` reverses one that would vanish. For symbols alone on a dark bar, pick lighter glyph colors.

## Color source

`color_source = "window"` paints the bar with a thin strip of the window's own content instead of `background`, so the bar takes on the application's color and follows it as the application repaints that edge. `color_source = "average"` paints the bar with that strip's mean color instead, one uniform color with none of the window's own detail, so a title bar, tab row or loading bar along that edge does not read as fragments of itself inside the bar. It costs a pixel readback when the window repaints that edge, which finishes the frame before reading it, and a burst of repaints pays for at most one readback per two of them, never closer together than a few milliseconds; the read covers the part of the edge the title spans, plus a small margin, wherever `title_align` puts it, instead of the whole bar, so leave it off for windows that repaint that edge every frame, such as video. `color_source_height` sets how many logical pixels that strip takes, from `1` to `4`; its default, `1`, samples the single top row and reads as one flat color, while a larger value keeps more of the window's own detail.

In `"window"` mode the bar is only ever the window's own pixels, so anything the application draws along that edge — a toolbar, a tab row, a loading bar — appears in it too; `"average"` reduces that detail to one color and shows none of it. A window whose top edge is transparent keeps the configured `background` there, because the bar color stays painted underneath, and the values in `[colors.title_bar]` remain the fallback for applications that draw nothing. Unfocused windows keep their own color, dimmed over `background_unfocused`. A client-drawn window that reports its own geometry keeps the transparent shadows around it out of the color, so the bar follows the visible window rather than its padding.

`"window"` mirrors the window's own buffer instead of reading pixels back, so it adds one bar-sized rectangle per window and a copy of that strip each time the window commits, and pays a readback only when `title_contrast_sample` asks for one, on the same cadence and over the union of the title and button spans rather than the whole bar. `"average"` does read that strip back on that cadence and at that width, once per commit that actually repaints the sampled region and once more when the bar's geometry, the strip height or the title changes, each finishing the frame first; the resulting color is a single pixel, so no further readback follows until that edge changes again. An explicit-sync window's strip is read only once its frame is reported ready, and read anyway after a brief wait when that report never arrives, so a stalled client cannot hold the bar on a stale color; a window that cannot be read — protected or external-only content — keeps the last color it gave, or the palette if it never gave one. The average is taken in the window's own color space and shown in it. An adaptive fill is opaque while the window is focused and the application's own edge is opaque; an unfocused bar draws it dimmed over `background_unfocused`, and a translucent top edge stays translucent, so the blurred backdrop shows through in both.

A [`title_bar_color_source`](window-rules-appearance.md#borders-corners-and-shadow) window rule sets `color_source` for one window, so the adaptive bar can be given to the applications that need it — a program with an ugly header bar, or a Wine application — while everything else keeps the palette. `color_source_height`, `title_shadow`, `title_contrast`, and `title_contrast_sample` stay global:

```toml
[appearance.title_bar]
color_source = "fixed"

[[window_rule]]
match.app_id = "^my-wine-app$"
title_bar_color_source = "window"
```

## Contrast

Because the bar can be any color the application paints, the title and the button symbols carry a dark edge in every `color_source`, so they stay readable on light and dark windows alike; `title_shadow = false` removes it from both. A bar that matches the title can still make it unreadable, so `title_contrast` replaces the title's color with black or white when it sits too close to the bar behind it, and leaves a color that already reads untouched. This applies in every `color_source`: `"fixed"` judges its palette against itself, and `"average"` its mean.

Symbols drawn straight on the bar, with `button_background = false`, follow the same rule against the edge under their own button; symbols on a painted button color are judged against that color as it is painted — at its state opacity over the bar beneath it, since a disabled or idle button shows the bar through itself — disabled ones included, and one with no alpha leaves the symbol on the bar. The comparison uses the bar's color as shown: the window's own edge dimmed over `background_unfocused` when the window is inactive, and the palette showing through a translucent edge. It is made before any transparency blend over the backdrop, and a `background` with no alpha at all is never judged, since it is not what shows.

`"window"` mirrors the window's pixels and knows no color of its own, so it does not reverse until `title_contrast_sample = true` reads that edge back once per change, which finishes the frame first — the cost `"window"` otherwise avoids; the same read covers each shown button that draws its symbol straight on the bar, so that symbol is judged against what sits under its own button. A transparent edge is a readable edge with no color in it, so the title is judged against the palette that shows through it; an edge that cannot be read at all keeps the last color the window gave, or leaves the title as configured if it never gave one.

## Buttons

- On narrow windows the padding and then the gap shrink, and the innermost buttons that still do not fit are left out.
- Maximize is disabled for fixed-size windows.
- Minimize shows only while the window can [minimize](window-states.md#minimize), so tiled and pinned windows have none.
- `button_glyph = "never"` with `button_background = false` hides the buttons and gives their space to the title.

```toml
# Only a close button on dialogs
[[window_rule]]
match.is_dialog = true
title_bar_buttons = ["close"]
```

## Using the title bar

- Drag empty bar space to move the window; double-click it to maximize or restore.
- Buttons act when released over the same button.
- Floating windows also resize from their borders and corners, with or without a bar.
- Windows in a [tab group](tab-groups.md) show the tab bar instead.
- Fullscreen hides the bar. Maximize and maximize-to-edges keep it, so the window can be restored with the mouse.
- The bar takes space inside the window's tile. Configured sizes, IPC geometry, and window captures describe only the application content.

## Blur behind the title bar

The bar follows its window's `blur` rules and the global [blur settings](appearance.md#blur). Give `background` and `background_unfocused` some transparency, such as `"#292933CC"`, to see the blur. A `color_source` of `"window"` or `"average"` paints the bar with the window's own pixels over it, opaque when that edge is opaque and the window is focused.

## Duplicate title bars

> [!NOTE]
> Umbriel cannot remove a title bar the application draws itself. Choose the application's system or native title bar option where it has one. `prefer_no_csd = true` in [`[appearance]`](appearance.md#windows) already asks apps that support it to leave decorations to Umbriel.
