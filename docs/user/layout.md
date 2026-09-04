# Layout

Choose one of Umbriel's three layout modes for each workspace: Scrolling,
Dwindle, or Master. Configure a default mode globally, then override it for
individual workspaces when needed. The initial scrolling width can also have an
output-specific default.

## Choose a layout

```toml
[layout]
mode = "scrolling" # "scrolling", "dwindle", or "master"
```

| Mode | Arrangement | Best suited to |
| ---- | ----------- | -------------- |
| `scrolling` | Columns form a scrollable strip. Each column can contain stacked windows. | Workflows that keep many windows available without shrinking every column. |
| `dwindle` | Each new window splits an existing tile. | Recursive tiling with independently sized regions. |
| `master` | Windows occupy a master area and a stack area. | Keeping one or more primary windows separate from the rest. |

The active mode can also be changed with `workspace-set-layout:<mode>`; see
[Actions](actions.md#argument-forms). A workspace rule can set a
persistent per-workspace mode; see [Workspace Rules](workspaces.md#workspace-rules).

Column-scoped actions operate on the active layout's column projection.
Scrolling owns real multi-window columns. In dwindle, every tiled leaf is a
single-window column, so column actions act on that window. In master, the
master and stack areas are the two columns. When an action has no meaning in
the active layout, its keybind does nothing and the IPC `msg` command returns
an error naming the required layout.

## Shared settings

```toml
[layout]
gap = 8
width_presets = [0.333, 0.5, 0.667]
```

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `gap` | int | `8` | Gap between windows in pixels (0-500). |
| `width_presets` | float array | `[0.333, 0.5, 0.667]` | Fractions used by `window-cycle-width` and `window-cycle-height` in every layout. Both actions use this one list. |

### Struts

```toml
[layout.struts]
left = 0
right = 0
top = 0
bottom = 0
```

Struts reserve signed logical pixels around the normal tiled layout. Umbriel
applies them after layer-shell exclusive zones, so positive values reserve more
space inward from panels, while negative values expand the tiled area and can
place windows beneath panels or beyond an output edge. Each edge accepts a value
from `-65535` to `65535`.

All three tiled layouts use the resulting area. On the scrolling axis, struts
leave room beyond the viewport where neighboring lanes can remain visible.
Floating windows and popups ignore struts. Maximize and fullscreen treat struts
differently from the tiled layout; see [Sizing behavior](#sizing-behavior).

`Mod+Right-drag` selects horizontal and vertical resize edges from the outer
thirds of tiled and floating windows. Dragging from a corner resizes both axes.
Mod+Right-click in the center starts no resize and preserves the window's
maximize state. For a tiled window, a center click also scrolls the focused
window into view. When a tiled resize ends, a focused scrolling column animates
back into view. In Dwindle, only edges backed by an internal split propose a
resize; screen-facing edges propose nothing.

## Scrolling layout

Scrolling keeps columns at their configured widths and moves the strip through a
viewport. A column can contain multiple windows. The `direction` setting changes
which screen axis is the strip axis.

### Settings

```toml
[layout.scrolling]
direction = "horizontal"             # "horizontal" or "vertical"
default_width_fraction = 0.5         # remove to let clients choose, 0.1-1.0
center_underfull_strip = true
center_focused = false
expand_single_column = true
```

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `direction` | string | `"horizontal"` | Strip axis: `"horizontal"` places columns left to right, while `"vertical"` places lanes top to bottom. |
| `default_width_fraction` | float | unset | Initial strip-axis extent for new columns (0.1-1.0). The packaged config sets `0.5`; a matching output or workspace rule can override it. When it is unset at every level, the client chooses its initial extent. |
| `center_underfull_strip` | bool | `true` | Center the complete strip when it is shorter than the viewport. Disable to align it at the start edge. |
| `center_focused` | bool | `false` | Always center the focused column. |
| `expand_single_column` | bool | `false` | Fill the viewport for a workspace's lone tiled column. Client size hints and viewport bounds still apply. The packaged config enables this. |

### Horizontal and vertical scrolling

| Direction | Layout | Width and height actions |
| --------- | ------ | ------------------------ |
| `horizontal` | Columns run left to right. Windows within a column stack from top to bottom. | Width actions change a column's strip extent. Height actions change a window's extent within its column. |
| `vertical` | Horizontal lanes run top to bottom. Windows within a lane sit side by side. | Width actions change a lane's strip extent, which is its height on screen. Height actions change a window's extent within its lane, which is its visual width. |

For a vertical workspace, directional actions follow the screen; see
[Vertical workspaces](#vertical-workspaces). The consume and expel actions use
the same visual directions: left and right merge or split within the lane, while
the resulting lane is above or below the focused lane.

The three-finger vertical swipe continues to switch workspaces. A three-finger
horizontal swipe scrolls a horizontal strip and is inert on a vertical one. Use
keyboard or wheel bindings to scroll a vertical strip. On a horizontal strip,
release velocity settles a three-finger swipe against a viewport edge.

### Scrolling behavior

The packaged config sets `default_width_fraction = 0.5`, so new columns start at
half the viewport. If the option is omitted, Umbriel leaves the strip-axis
extent unconstrained during the initial configure and retains the logical size
chosen by the client. A numeric `default_width` window rule takes precedence.
For a new horizontal column, the pixel width from a matching `default_size`
window rule takes precedence over both fractional settings.

Set a different initial width for every scrolling workspace on one output under
that output's section:

```toml
[output.DP-1.layout.scrolling]
default_width_fraction = 0.4

[output."Microstep MSI G2712F CD6T084401192".layout.scrolling]
default_width_fraction = 0.6
```

Connector and monitor names follow the normal [output identity](outputs.md)
rules. A monitor-named output section wins when both it and a connector section
match the same display.

The initial width is resolved from the global setting, then the matching output
setting, then a matching workspace rule without an `output`, and finally a
matching workspace rule with an `output`. Each later value takes precedence.
Only `default_width_fraction` has this output-level layout override; the other
layout settings remain global or per workspace.

Changing any of these defaults on reload affects columns created afterward. It
does not resize existing columns, and a column moved to another output retains
its stored fraction. Re-tiling a floating window or expelling a window into a
new column creates a column using the current default.

`expand_single_column` affects only how a lone tiled column is displayed. It
does not rewrite the stored fraction, so the configured or client-selected
width applies again when a second column appears. Explicit
`default_maximize` and `default_maximize_to_edges` window rules take precedence.

When focus moves to a hidden or partially hidden column, Umbriel reveals it by
the shortest distance needed to show it completely. A column entering from the
right aligns with the right viewport edge, and one entering from the left aligns
with the left edge. Focus does not reserve a visible sliver for the next column.
Resizing a column recenters an underfull strip immediately.

Dragged windows become translucent so the insertion preview remains visible.
Existing window transparency still applies during the drag. When a column is
dragged, the preview uses the free space beside the real column edges. If the
strip extends beyond the output, its far left and right edges remain available
as prepend and append targets even when the end columns are off-screen.

Dropping a window into empty space above or below a vertically resized stack
consumes that space. Existing windows retain their pixel heights, and the
dropped window fills the remainder apart from the configured gap.

## Vertical workspaces

On a vertical scrolling workspace, directional actions follow their visual
directions. `window-focus-left` and `window-focus-right` move within a lane,
while `window-focus-up` and `window-focus-down` walk lanes. Likewise,
`column-move-left` and `column-move-right` reorder within a lane, while
`window-move-up` and `window-move-down` move the lane along the strip.
`layout-scroll-left` and `layout-scroll-up` both scroll toward strip start;
their right and down forms scroll toward strip end.

The default Mod+wheel bindings invoke `window-focus-left` and
`window-focus-right`, so they move within a lane. Vertical-heavy configurations
should bind wheel chords to `window-focus-up` and `window-focus-down`, or to
`layout-scroll-up` and `layout-scroll-down`.

## Dwindle layout

Dwindle recursively splits tiles into independently sized regions.

### Settings

```toml
[layout.dwindle]
preserve_split = false # keep each split direction fixed after creation
```

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `preserve_split` | bool | `false` | Keep each split direction fixed after it is created. |

### Behavior

A new window splits the focused tile along its longer edge and becomes the right
or bottom half. With `preserve_split = false`, split directions follow each
tile's shape as geometry changes, so closing a window can reflow surviving
splits. Dropping on a specific edge keeps that direction. Set
`preserve_split = true` to freeze every split direction when it is created.

When a window or column moves onto a Dwindle workspace, a multi-window column is
flattened into ordered single-window columns. This is also how drag-and-drop
handles a column that enters Dwindle.

## Master layout

Master divides the workspace into a master area and a stack area. The master
area is on the side selected by `position`; the stack occupies the other side.
Each area arranges its windows from top to bottom. When only one area has
windows, that area fills the complete content box.

### Settings

```toml
[layout.master]
position = "left"                   # "left" or "right"
default_width_fraction = 0.55       # 0.1-0.9
new_on_top = true                    # place new windows at the top of the stack
```

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `position` | string | `"left"` | Side occupied by the master area: `"left"` or `"right"`. |
| `default_width_fraction` | float | `0.55` | Initial fraction assigned to the master area when both areas exist (0.1-0.9). |
| `new_on_top` | bool | `true` | Place new windows at the top of the stack. Disable to place them at the bottom. |

### Behavior

The first window becomes master. A new window also becomes master when the
master area is empty. Otherwise it joins the top of the stack when
`new_on_top = true`, or the bottom when it is false. Removing the final master
window promotes the top stack window. Moving every window out of master does
not promote one, so the remaining stack stays full-width until another window
opens or is moved into master.

Consume actions preserve their visual meanings. With `position = "left"`, left
moves a stack window into master and right moves a master window into the stack.
With `position = "right"`, those roles reverse because master is visually right.
The consume-or-expel variants make the same directional move.

Master workflows use a deterministic layout-order ring: master windows from top
to bottom, then stack windows from top to bottom. `window-focus-next` and
`window-focus-previous` cycle through that ring and wrap. The swap actions
exchange the focused window with its neighboring slot while keeping focus on
that window. `layout-master-count-increase` promotes the stack top into master,
and `layout-master-count-decrease` demotes the master bottom into the stack. At
least one window remains in master.

Width actions operate on the master fraction; the stack fraction is its
complement. `window-modify-width:<delta>` changes the focused area's fraction,
and the cycle actions walk `width_presets`. Width actions are inert while either
area is empty because the occupied area already fills the viewport. Height
actions change a window's row fraction within its area; see
[Sizing behavior](#sizing-behavior). Tiled resizing is available on the boundary
between master and stack and between rows in either area.

Dragging over a master workspace previews the destination row within the
nearest area. Hint bands appear at the top, bottom, and between existing rows.
Dropping inserts the window at that row.

## Sizing behavior

The width and height actions are shared by every layout, but each layout gives
them a different meaning.

`window-modify-width:<delta>` changes the focused area's width by a signed
fraction, and `window-cycle-width` / `window-cycle-width-back` walk
`width_presets` in either direction.

`window-set-height:<frac>` sets the focused window's fraction of its column's
stacking extent, `window-modify-height:<delta>` changes that fraction by a
signed amount, and `window-cycle-height` / `window-cycle-height-back` cycle it
through the same presets in either direction. In scrolling and master layouts
this sizes a row within its column or area. In dwindle it adjusts the vertical
splits containing the window. On a vertical scrolling workspace the stacking
axis is horizontal, so these actions change a window's width within its lane.

In the scrolling layout, a window alone in its column is resized from its bottom
edge, exactly as dragging that edge does: the top edge stays where it is and the
freed space collects below the window, so the next window stacked into that
column fills it. A fraction of `1.0` reclaims the space. A window that a
previous drag pushed against the column's bottom keeps that anchor and frees
space above itself instead. In master and dwindle, a window with no neighbor on
the stacking axis has nothing to trade space with, so the height actions leave
it unchanged.

### Floating windows

All of the width and height actions resize a focused floating window directly,
as fractions of the output's usable area clamped to the client's min/max size
hints. Cycling walks `width_presets` on either axis, stepping to the next preset
that changes the window's pixel size on that axis: a float's size is pixels, so
a preset that rounds to the size the window already has is skipped rather than
applied as a step that does nothing. Resizing a maximized float leaves
maximization behind and keeps the new size, so a later toggle maximizes rather
than reverting to the pre-maximize box. Both axes use the
`animation.windows_move` transition, and a float that hangs off an edge travels
with the resize so the same part of it stays on screen at the new size.
Fullscreen owns the size outright, so the actions do nothing while a float is
fullscreen.

Parented XDG dialogs are stacked with their ancestor chain. Raising any member
raises the family while keeping each dialog above its parent, including when an
ancestor is floating, pinned, or fullscreen.

### Maximize and fullscreen

`window-toggle-fullscreen` ignores layout struts and layer-shell exclusive zones
and fills the entire output. It normally targets the focused window. If another
fullscreen window completely covers that focus on the active output, the action
exits the covering fullscreen window first and leaves focus in place.
`window-toggle-maximize` toggles the focused column's full-width state, and a
tiled column stays inside configured struts and gaps. A floating window has no
column, so it fills the output's usable area and restores its exact previous
box, including its last dropped position. Both directions use the
`animation.windows_move` transition.
`window-toggle-maximize-to-edges` drops layout struts, gaps, and borders, while
layer-shell exclusive zones remain visible. A column's full-width restore state
survives that toggle and a fullscreen round trip. In the scrolling layout the
strip reserves the strut band such a column reaches past, so neighboring
columns keep their gap instead of sitting underneath the window.
