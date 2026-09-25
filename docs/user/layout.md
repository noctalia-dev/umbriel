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
```

| Key | Default | Description |
| --- | --- | --- |
| `gap` | `8` | Gap between windows in logical pixels. |
| `extent_presets` | `[0.333, 0.5, 0.667]` | Fractions used by primary and secondary extent cycle actions. |

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
new_exits_fullscreen = false
```

| Key | Default | Description |
| --- | --- | --- |
| `preserve_split` | `false` | Keep each split direction fixed after creation. |
| `new_exits_fullscreen` | `false` | Exit fullscreen when a new window opens. |

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
new_exits_fullscreen = false
```

| Key | Default | Description |
| --- | --- | --- |
| `position` | `"left"` | Use `"left"`, `"right"`, or `"center"`. |
| `default_width_fraction` | `0.55` | Initial master-area fraction. |
| `new_on_top` | `true` | Put new stack windows at the top. |
| `new_becomes_master` | `false` | Give the master slot to each new window. |
| `new_exits_fullscreen` | `false` | Exit fullscreen when a new window opens. |

### Behavior

The first window becomes master. Later windows join the stack unless
`new_becomes_master` is enabled. Use
`layout-master-count-increase` and `layout-master-count-decrease` to move
windows between the two areas.

In center mode, stack windows are balanced between the left and right sides.
Primary extent actions resize the master area. Secondary extent actions resize
rows within an area.

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
