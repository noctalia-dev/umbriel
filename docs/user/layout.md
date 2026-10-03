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

During an application drag-and-drop operation, holding the pointer near either
edge of the scrolling axis reveals offscreen columns after a short delay. The
strip moves faster as the pointer approaches the edge, and stops when the
pointer moves away or the drag ends. A window revealed beneath a stationary
pointer becomes the active drop target, so the file or other payload can be
dropped without extra pointer motion.

With `center_focused = "on_overflow"`, a column is centered when it and the
neighbor focus came from cannot share the viewport, and a width change that
makes them fit puts the pair back side by side at the edge focus came from.
Closing the focused column or sending it to another workspace runs the same
rule with the column that took its place. Moving or resizing the focused column
does too, as does going fullscreen or maximized to edges. A lone column has no
such pair, so it never centers itself; `center_underfull_strip` decides where it
rests.

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
new_toward_cursor = false
```

| Key | Default | Description |
| --- | --- | --- |
| `preserve_split` | `false` | Keep each split direction fixed after creation. |
| `new_toward_cursor` | `false` | Place a new window on the half of the focused tile nearer the pointer. |

### Behavior

A new window splits the focused tile along its longer edge. Without
`preserve_split`, split directions may adapt as geometry changes. Enable it for
stable, manually shaped regions.

The new window takes the right or bottom half of the split by default. With
`new_toward_cursor`, it takes the half on the pointer's side of the tile's
center instead, so the pointer stays over the new window when it was over the
focused tile. The focused tile is still the one that splits, and the split axis
does not change. When the pointer is on another output or the overview is
open, the default applies.

Directional moves (`window-move-left`, `window-move-right`, `window-move-up`,
`window-move-down`, their `-or-output` and `-or-workspace` variants, and
`column-move-left` and `column-move-right`, which do the same here) take
the window out of its tile and split the tile it moves into. That tile splits
along its longer edge, and the window takes the half nearer where it came
from. The tile it left goes back to its sibling, so windows it passes through
only shrink while it is inside them and keep their place in the layout. Moving
toward a single neighbor that shares its split trades sides with that neighbor.
To exchange two windows without changing any tile, use the `window-swap-*`
actions.

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

## Tab groups

A tab group gives each of its windows the same box and shows one of them, the
active tab. A bar along one edge of the group has a slot for every window, and
the active tab's slot is highlighted. In the scrolling layout a group is one
row of its column: it stacks above and below other windows, takes a row's
share of the column, and resizes like any row. A column can hold several
groups, and a group can hold every window of its column. In the master layout
the master area and each stack are tabbed as a whole. Dwindle tiles hold one
window each, so they cannot be tabbed.

`column-toggle-tabbed` (built in as `Mod+W`; bind it under `[keybinds]` when
your config lists its own) tabs the focused window together with the windows
stacked next to it, up to the nearest group above and below; in a column with
no groups that is the whole column. On a tab it stacks its group's windows
again. `column-set-display:<normal|tabbed>` does either one. The focused window
becomes the active tab.

| To | Do |
| --- | --- |
| Switch tabs | Click a slot, scroll over the bar, use `column-focus-tab-next` and `column-focus-tab-previous`, or jump with `column-focus-tab:<index>` |
| Add a tab | Move a window into the group with `window-move-up` or `window-move-down`, consume it into a column whose last row is a group, pull the neighboring column's window in with `window-consume-from-left` or `window-consume-from-right` while a tab is focused, or drop it on the group's bar or middle |
| Put a window above or below a group | Move a tab out with `window-move-up` or `window-move-down`, or drop a window near the group's top or bottom edge |
| Remove a tab | Move it out, drag its slot out of the bar, expel it, float it, or move it away |
| Reorder tabs | Use `column-move-tab-next` and `column-move-tab-previous`, or drop a window between two slots |
| Resize a group | Drag the edge between it and the window beside it, or use the secondary extent actions on any of its tabs |
| Hide or show its bar | Use `column-toggle-tab-bar`, `column-hide-tab-bar`, or `column-show-tab-bar` |
| Close a tab | Middle-click its slot, when `middle_click_closes` is on |

In the scrolling layout `window-move-up` and `window-move-down` treat a group as
one row. A tab steps out of its group toward the move and keeps the group's
height. A window standing alone steps into a group it meets, as its nearest
tab. A group of one tab moves past its neighbour like a window. Focus moves
within a column step over a group and land on its active tab. In a tabbed
master area they step through its tabs instead.

Focusing a window by any means shows its tab: a keybind, a click, activation,
or IPC. Moving focus into a group from outside it lands on the active tab.
Hidden tabs stay mapped and sized to the group, so switching tabs is instant and
never resizes a window. A hidden tab is told it is suspended, so an application
that honours it, such as recent GTK and Chromium-based ones, stops drawing until
its tab shows again; it still receives frame callbacks at the rate of windows
on hidden workspaces, for those that do not. A hidden tab that asks for
attention lights its slot with the urgent color.

A group whose bar is hidden still works: its tabs take the bar's space, and the
tab actions and focus moves still switch between them. Hiding the bar of one
group leaves the others alone. `visible` under
[`[appearance.tab_bar]`](appearance.md#tab-bar) hides every bar a group has not
shown itself.

Dropping a window onto a bar inserts it between the two slots under the
pointer. Dropping it on the middle half of a group adds it where new tabs go;
near the group's top or bottom it lands beside the group instead. A master area
that empties starts over with `default_display` when a window next arrives.

```toml
[layout.tabs]
default_display = "normal"
new_tab_position = "end"
wrap_focus = true
scroll_switches_tabs = true
middle_click_closes = false
```

| Key | Default | Description |
| --- | --- | --- |
| `default_display` | `"normal"` | How new columns and master areas show their windows: `"normal"`, or `"tabbed"` to start each as a tab group. |
| `new_tab_position` | `"end"` | Where a consumed, pulled, or dropped window joins a tab group: `"end"`, or `"after_active"` beside the active tab. |
| `wrap_focus` | `true` | Wrap `column-focus-tab-next` and `column-focus-tab-previous` at the ends. |
| `scroll_switches_tabs` | `true` | Step through tabs with the wheel over the bar. |
| `middle_click_closes` | `false` | Close a tab with a middle click on its slot. |

A `[[workspace]]` entry's `layout.tabs` table overrides these keys for that
workspace. A window rule's `default_column_display` sets the display of the
column a window opens in. See [Tab bar](appearance.md#tab-bar) for the bar's
look and space.

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
