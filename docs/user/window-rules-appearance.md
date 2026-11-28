# Appearance rules

Appearance keys override the global look for the windows a rule matches. They update while the window is open and on config reload.

```toml
# Blur every window
[[window_rule]]
blur = true
```

## Appearance keys

### Transparency and blur

| Key                 | Default | Description                                                                               |
| ------------------- | ------- | ----------------------------------------------------------------------------------------- |
| `opacity`           | `1.0`   | Surface opacity, 0.0 to 1.0.                                                              |
| `blur`              | off     | Blur behind the window, including a translucent Umbriel title bar.                        |
| `blur_popups`       | off     | Also blur the window's popups. Popups always blur the live content behind them.           |
| `blur_ignore_alpha` | `0.0`   | Skip blur where the content is more transparent than this, 0.0 to 1.0.                    |
| `blur_optimized`    | global  | Override `appearance.blur.optimized`.                                                     |
| `dim_behind`        | `0.0`   | Darken what is below the window on its output, 0.0 to 1.0. See [Dim behind](#dim-behind). |

### Borders, corners, and shadow

| Key                      | Default | Description                                                                                                                         |
| ------------------------ | ------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| `border_width`           | global  | Override `appearance.border_width`, 0 to 100.                                                                                       |
| `outer_border_width`     | global  | Override `appearance.outer_border_width`, 0 to 100.                                                                                 |
| `border_color_focused`   | global  | Override `colors.border.focused`.                                                                                                   |
| `border_color_unfocused` | global  | Override `colors.border.unfocused`.                                                                                                 |
| `border_color_outer`     | global  | Override `colors.border.outer`.                                                                                                     |
| `corner_radius`          | global  | Override `appearance.corner_radius`, 0 to 100.                                                                                      |
| `shadow`                 | global  | Override `appearance.shadow.enabled`.                                                                                               |
| `title_bar`              | global  | `"on"`, `"negotiated"`, `"forced"`, or `"off"`; overrides `appearance.title_bar.mode`.                                              |
| `title_bar_buttons`      | global  | Override `appearance.title_bar.buttons`: any of `"minimize"`, `"maximize"`, `"close"` from the title outward, or `[]` for none.     |
| `title_bar_color_source` | global  | `"fixed"`, `"window"`, or `"average"`; overrides `appearance.title_bar.color_source`. See [Title bars](title-bars.md#color-source). |

### Effects

| Key                       | Default | Description                                                        |
| ------------------------- | ------- | ------------------------------------------------------------------ |
| `border_effect`           | global  | Replace `effects.border` with a border preset or pool, or `"off"`. |
| `border_effect_unfocused` | global  | Override `effects.border_unfocused` with `true` or `false`.        |
| `window_effect`           | global  | Replace `effects.window` with a window preset or pool, or `"off"`. |

> [!NOTE]
> Fullscreen windows ignore rule `opacity` unless `appearance.opaque_fullscreen = false`. See [Windows](appearance.md#windows).

## Fade unfocused windows

```toml
# Unfocused windows at 85% opacity
[[window_rule]]
match.is_focused = false
opacity = 0.85

[[window_rule]]
match.is_focused = true
opacity = 1.0
```

Opacity and blur fade when focus changes. The fade timing is set in [Window opacity](animation.md#window-opacity).

## Borders that show window state

Each `border_color_*` key is independent: an unset one keeps the global color.

```toml
# Amber borders on scratchpad windows
[[window_rule]]
match.is_scratchpad = true
border_color_focused = "#E5C07BFF"
border_color_unfocused = "#5C4A2AFF"
```

`match.is_only_window` holds while no other window is on the workspace:

```toml
# No borders when a window is on its own
[[window_rule]]
match.is_only_window = true
border_width = 0
outer_border_width = 0
```

- Floating windows count; pinned and scratchpad windows do not.
- It does not drive fullscreen or maximize. Those follow `match.is_alone`; see [Rules for a lone window](window-rules-placement.md#rules-for-a-lone-window).

`match.is_cast_target` holds while the window itself is captured:

```toml
# A green frame around the window being shared
[[window_rule]]
match.is_cast_target = true
border_width = 5
border_color_focused = "#40C057FF"
border_color_unfocused = "#40C057FF"
```

- It matches window shares and window screenshots, not whole-output shares.
- The frame is not part of the captured image, so viewers never see it.

## Title bars

```toml
# Title bars only in the stacking layout
[[window_rule]]
match.layout = "stacking"
title_bar = "on"
```

This also covers windows floated by hand in stacking, without adding bars to floating windows in other layouts.

```toml
# Always a bar on the editor, except its Preferences window
[[window_rule]]
match.app_id = "^my-editor$"
title_bar = "forced"

[[window_rule]]
match.app_id = "^my-editor$"
match.title = "^Preferences$"
title_bar = "on"
```

```toml
# Hide the bar while maximized to the edges
[[window_rule]]
match.is_maximized_to_edges = true
title_bar = "off"
```

- A rule overrides the global mode in either direction; an omitted key inherits it.
- Fullscreen and tab groups still hide the bar.
- Forcing a bar does not remove the application's own headerbar.

The modes and the bar's look are described in [Title bars](title-bars.md).

### Color source

```toml
# Take the bar's color from the window's own edge, for these only
[[window_rule]]
match.app_id = "^my-wine-app$"
title_bar_color_source = "window"
```

`title_bar_color_source` sets [`color_source`](title-bars.md#color-source) for one window, so the bar can take its color from the window's own top edge only where that helps — a program with an ugly header bar, or a Wine application — while terminals and editors keep the palette. It works in both directions: `"window"` or `"average"` opts a window in over a global `"fixed"`, and `"fixed"` exempts one from a global `"window"`. A later matching rule replaces an earlier override, while a rule that omits the key leaves one in place. `color_source_height`, `title_shadow`, `title_contrast`, and `title_contrast_sample` stay global.

## Applications with their own decorations

```toml
# The app draws its own corners and shadow
[[window_rule]]
match.app_id = "^org[.]gnome[.]TextEditor$"
border_width = 0
corner_radius = 0
shadow = false
```

> [!NOTE]
> Rule border widths do not change layout spacing. A thinner border leaves its gap empty, and a thicker one draws into the gap.

## Transparent content and blur

- `blur_ignore_alpha = 0.01` skips blur under fully or nearly transparent pixels, while still blurring translucent content.
- Some applications clear their content just before closing a window, which can leave a blurred rectangle for a moment. `blur_optimized = false` blurs the live content behind it instead of the wallpaper.
- If a shadow outline remains, add `shadow = false`. This removes the shadow from those windows entirely.

```toml
# Blur only where the app draws something
[[window_rule]]
match.app_id = "^example-app$"
blur = true
blur_ignore_alpha = 0.01
```

## Effects

```toml
# Scanlines on mpv, without the border effect
[[window_rule]]
match.app_id = "^mpv$"
border_effect = "off"
window_effect = "scanlines"
```

Names refer to `[effects.preset.<name>]` or `[effects.pool.<name>]` tables of the matching kind; see [Effects](effects.md#turn-a-default-off-for-one-window-or-output). A window keeps its pick from a pool across rule changes, and runtime overrides take precedence over rules ([Pools](effects.md#pools)).

## Dim behind

```toml
# Darken everything behind the launcher
[[window_rule]]
match.app_id = "^launcher$"
dim_behind = 0.5
```

- Content below the window on its output darkens; the window, its popups, and anything above it do not.
- It is suppressed in the overview and does not block pointer input.
- Several dimming windows stack.
- Add `match.is_focused = true` to dim only while the window is focused.
