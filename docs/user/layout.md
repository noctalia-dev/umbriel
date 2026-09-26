# Layout

Choose one layout mode for each workspace. Configure the default globally, then
use workspace rules for exceptions.

## Choose a layout

```toml
[layout]
mode = "scrolling"
```

| Mode | Arrangement | Best suited to |
| --- | --- | --- |
| `scrolling` | Fixed-size columns in a scrollable strip | Keeping many windows readable without shrinking all of them |
| `dwindle` | Each new window splits an existing tile | Flexible recursive tiling |
| `master` | Primary windows beside a stack | Keeping one or more main windows prominent |

Change the current workspace at runtime with
`workspace-set-layout:<mode>`. See [Actions](actions.md#argument-forms) and
[Workspace Rules](workspaces.md#workspace-rules).

## Shared settings

```toml
[layout]
gap = 8
extent_presets = [0.333, 0.5, 0.667]
new_exits_fullscreen = []  # "tiled", "floating", "pinned", "all", or an array such as ["tiled", "floating"]
```

| Key | Default | Description |
| --- | --- | --- |
| `gap` | `8` | Gap between windows in logical pixels. |
| `extent_presets` | `[0.333, 0.5, 0.667]` | Fractions used by primary and secondary extent cycle actions. |
| `new_exits_fullscreen` | `[]` | Kinds of arriving window that make a fullscreen window on the workspace leave fullscreen. See [Leaving fullscreen](#leaving-fullscreen). |

### Leaving fullscreen

`new_exits_fullscreen` selects which kinds of window make a fullscreen window
leave fullscreen when they arrive on its workspace. A window arrives when it
opens there, is moved there from another workspace or output, is dropped there
by drag-and-drop, or returns there from a scratchpad.

| Value | Arriving window |
| --- | --- |
| `"tiled"` | A tiled window in the Dwindle or Master layout. |
| `"floating"` | A floating window that is not pinned. |
| `"pinned"` | A pinned window. |
| `"all"` | Any window. |

A string selects one kind and an array selects several. The empty array, the
default, disables the behavior. In the scrolling layout a tiled window opens as
a column beside the fullscreen one and the strip scrolls to it, so it never
exits fullscreen.

### Struts

```toml
[layout.struts]
left = 0
right = 0
top = 0
bottom = 0
```

Positive struts reserve extra space inside layer-shell exclusive zones.
Negative values let tiled windows extend beneath panels or beyond an output
edge. Floating windows and popups ignore struts.

All layouts support `Mod+Right-drag` resizing. Drag from an edge to resize one
axis or from a corner to resize both.

## Scrolling layout

Scrolling keeps columns at their configured extents and moves the strip through
the output. A column can contain several stacked windows.

The strip is perpendicular to the output's
[workspace axis](workspaces.md#workspace-axis). Vertical workspaces use a
horizontal strip; horizontal workspaces use a vertical strip.

### Settings

```toml
[layout.scrolling]
default_extent_fraction = 0.5
center_underfull_strip = true
center_focused = "never"
```

| Key | Default | Description |
| --- | --- | --- |
| `default_extent_fraction` | unset | Initial column extent from 0.1 to 1.0. The packaged config uses `0.5`. |
| `center_underfull_strip` | `true` | Center a strip narrower than the viewport. |
| `center_focused` | `"never"` | Use `"never"`, `"always"`, or `"on_overflow"` to control focus centering. |

### Horizontal and vertical scrolling

| Workspace axis | Scrolling arrangement |
| --- | --- |
| `vertical` | Columns run left to right; windows stack top to bottom. |
| `horizontal` | Lanes run top to bottom; windows sit left to right. |

Primary extent actions resize a column along the strip. Secondary extent
actions resize a window within its column.

A three-finger swipe along the workspace axis switches workspaces. A swipe
across it scrolls the strip. Touchpad direction follows
`input.touchpad.natural_scroll`.

### Scrolling behavior

When `default_extent_fraction` is unset, applications choose their initial
extent. A `default_scrolling_extent` window rule overrides the fraction, and
`default_scrolling_extent_px` takes highest precedence.

Set an output-specific default with:

```toml
[output.DP-1.layout.scrolling]
default_extent_fraction = 0.4
```

A workspace rule can override both global and output defaults. Reloading these
settings affects new columns only.

When focus moves to a hidden column, Umbriel scrolls just far enough to reveal
it. Dragged windows show an insertion preview and can be dropped into a new or
existing column.

Closing a focused column moves focus to the nearest surviving column. When that
column contains stacked windows, Umbriel restores its most recently focused
member instead of always selecting its first row.

With `follows_mouse = true`, closing a focused window beneath the pointer instead
focuses the tiled window that occupies that position after the layout reflows.
This also applies when another row in the same scrolling column expands into the
stationary pointer.

## Vertical strips

With horizontal workspaces, screen directions remain literal:

- Left and right actions move within a lane.
- Up and down actions move between lanes.
- Strip scroll actions toward up or left move toward strip start.

Configurations using vertical strips usually bind wheel navigation to
`window-focus-up` and `window-focus-down` instead of the default left and right
actions.

## Dwindle layout

Dwindle recursively splits tiles into independently sized regions.

### Settings

```toml
[layout.dwindle]
preserve_split = false
```

| Key | Default | Description |
| --- | --- | --- |
| `preserve_split` | `false` | Keep each split direction fixed after creation. |

### Behavior

A new window splits the focused tile along its longer edge. Without
`preserve_split`, split directions may adapt as geometry changes. Enable it for
stable, manually shaped regions.

Dwindle has no multi-window columns. Moving one into Dwindle places its windows
as separate tiles.

## Master layout

Master places one or more primary windows in a master area and the remaining
windows in a stack. The master area may sit left, right, or between two stacks.

### Settings

```toml
[layout.master]
position = "left"
default_width_fraction = 0.55
new_on_top = true
new_becomes_master = false
```

| Key | Default | Description |
| --- | --- | --- |
| `position` | `"left"` | Use `"left"`, `"right"`, or `"center"`. |
| `default_width_fraction` | `0.55` | Initial master-area fraction. |
| `new_on_top` | `true` | Put new stack windows at the top. |
| `new_becomes_master` | `false` | Give the master slot to each new window. |

### Behavior

The first window becomes master. Later windows join the stack unless
`new_becomes_master` is enabled. Use
`layout-master-count-increase` and `layout-master-count-decrease` to move
windows between the two areas.

In center mode, stack windows are balanced between the left and right sides.
Primary extent actions resize the master area. Secondary extent actions resize
rows within an area.

## Tabbed columns

A tabbed column gives all of its windows the whole column. Only one window, the
active tab, is shown. A bar across the top of the column lists every window by
title, and the active tab is highlighted. In the scrolling layout this works on
columns. In the master layout it works on the master area and on each stack.
Dwindle tiles hold a single window, so they cannot be tabbed.

`column-toggle-tabbed` (default `Mod+W`) switches the focused window's column
between stacked and tabbed. The focused window becomes the active tab.

| To | Do |
| --- | --- |
| Switch tabs | Click a tab, use `window-focus-up` and `window-focus-down`, or use `column-focus-tab-next` and `column-focus-tab-previous` (these wrap) |
| Add a tab | Consume a window into the column, or drop one onto it |
| Remove a tab | Expel the window, float it, or move it away |
| Reorder tabs | Use `window-move-up` and `window-move-down` |

Focusing a window by any means shows its tab: a keybind, a click, activation, or
IPC. Moving focus into a tabbed column from outside it lands on the active tab.
Hidden tabs stay mapped at the column's size, so switching tabs is instant and
never resizes a window. Tabs have no rows, so secondary extent actions and
row resizing have no effect in a tabbed column. A master area stays tabbed
after its last window leaves, so the next window to arrive becomes a tab.

```toml
[layout.tabs]
bar_height = 24
font_size = 10
```

| Key | Default | Description |
| --- | --- | --- |
| `bar_height` | `24` | Height of the tab bar in logical pixels, from 12 to 96. The column also keeps one `gap` below it. |
| `font_size` | `10` | Tab title font size in points, from 6 to 48. |

[Tab bar colors](appearance.md#tab-bar-colors) set the bar's colors.

## Sizing behavior

Primary and secondary extent actions use `layout.extent_presets`:

- Scrolling: primary changes column extent; secondary changes a row.
- Dwindle: primary adjusts horizontal splits; secondary adjusts vertical splits.
- Master: primary changes the master fraction; secondary changes a row.
- Floating: primary changes width; secondary changes height.

The `window-set-*` actions assign an exact fraction. `window-modify-*` changes
it by a signed amount, and `window-cycle-*` walks the configured presets.

### Client minimum sizes

Applications may enforce a minimum size larger than their assigned tile.
Umbriel clips oversized content rather than allowing it to cover neighboring
tiles. Application-specific minimums must be disabled in that application's
settings when smaller tiles are required.


### Floating windows

For floating windows, extent fractions use the output's usable area and respect
the application's minimum and maximum size hints. Resizing a maximized floating
window leaves maximization and keeps the new size. Extent actions do nothing
while the window is fullscreen.

Parented dialogs stay above their parent and normally open centered over its
visible area.

### Maximize and fullscreen

`window-toggle-fullscreen` fills the complete output, ignoring struts and panel
exclusive zones.

`window-toggle-maximize` fills the layout area. Tiled columns keep struts and
gaps; floating windows fill the output's usable area.

`window-toggle-maximize-to-edges` removes layout struts, gaps, and borders while
leaving panel exclusive zones visible.
