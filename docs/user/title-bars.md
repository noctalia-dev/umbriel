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
```

| Key                    | Default                             | Description                                                                                                |
| ---------------------- | ----------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| `mode`                 | `"off"`                             | See [Modes](#modes).                                                                                       |
| `height`               | `28`                                | Bar height in logical pixels, 16 to 128.                                                                   |
| `font`                 | `"sans 10"`                         | Pango font description.                                                                                    |
| `title_align`          | `"left"`                            | `"left"`, `"center"`, or `"right"`. Centered titles move aside only as far as needed to clear the buttons. |
| `button_position`      | `"right"`                           | `"left"` or `"right"`.                                                                                     |
| `buttons`              | `["minimize", "maximize", "close"]` | Buttons from the title outward; the last sits at the edge. Each may appear once; `[]` for none.            |
| `button_size`          | `-1`                                | Button size in logical pixels, 1 to `height`; `-1` derives it from `height`.                               |
| `button_gap`           | `8`                                 | Space between buttons, 0 to 100.                                                                           |
| `button_padding`       | `-1`                                | Space between the buttons and the bar's edge, 0 to 100; `-1` matches the space above the buttons.          |
| `button_corner_radius` | `-1`                                | Button corner radius, 0 to 100; `-1` follows the window's `corner_radius`. Large values make circles.      |
| `button_glyph`         | `"hover"`                           | When to draw the button symbols: `"always"`, `"hover"` (while hovered or pressed), or `"never"`.           |
| `button_glyph_size`    | `-1`                                | Symbol size in logical pixels, 1 to 128; `-1` is half the button.                                          |
| `button_glyph_stroke`  | `-1`                                | Symbol line width, 0.5 to 16; `-1` is a twelfth of the button.                                             |
| `button_background`    | `true`                              | Draw colored button backgrounds; `false` leaves only the symbols.                                          |

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

Symbol colors are used as given. For symbols alone on a dark bar, pick lighter glyph colors.

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

The bar follows its window's `blur` rules and the global [blur settings](appearance.md#blur). Give `background` and `background_unfocused` some transparency, such as `"#292933CC"`, to see the blur.

## Duplicate title bars

> [!NOTE]
> Umbriel cannot remove a title bar the application draws itself. Choose the application's system or native title bar option where it has one. `prefer_no_csd = true` in [`[appearance]`](appearance.md#windows) already asks apps that support it to leave decorations to Umbriel.
