# Layout

This page covers scrolling, dwindle, and master layout configuration and behavior.

## Settings and behavior

```toml
[layout]
mode = "scrolling"                  # "scrolling", "dwindle", or "master"
gap = 8                             # 0-500
width_presets = [0.333, 0.5, 0.667]

[layout.scrolling]
direction = "horizontal"             # "horizontal" or "vertical"
default_width_fraction = 0.5         # remove to let clients choose, 0.1-1.0
center_underfull_strip = true
expand_single_column = true           # fill lone column to viewport width

[layout.dwindle]
preserve_split = false              # keep each split direction fixed after it is created

[layout.master]
position = "left"                   # "left" or "right"
default_width_fraction = 0.55       # 0.1-0.9
```

Shared layout options:

| Key             | Type        | Default               | Description                                                        |
| --------------- | ----------- | --------------------- | ------------------------------------------------------------------ |
| `mode`          | string      | `"scrolling"`         | Layout algorithm: `"scrolling"`, `"dwindle"`, or `"master"`.       |
| `gap`           | int         | `8`                   | Gap between windows in pixels (0-500).                             |
| `width_presets` | float array | `[0.333, 0.5, 0.667]` | Widths visited by `window-cycle-width` in every layout.            |

Scrolling layout options:

| Key                      | Type   | Default        | Description                                                                                                                       |
| ------------------------ | ------ | -------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| `direction`              | string | `"horizontal"` | Scroll axis: `"horizontal"` stacks columns left to right; `"vertical"` stacks lanes top to bottom.                                |
| `default_width_fraction` | float  | unset          | Initial scroll-axis extent assigned to new scrolling lanes (0.1-1.0). The packaged config sets `0.5`; when omitted, the client chooses its initial extent. |
| `center_underfull_strip` | bool   | `true`         | Center the complete strip whenever it is shorter than the viewport. Disable to align it at the start edge.                        |
| `expand_single_column`    | bool   | `false`        | Fill the viewport width for a workspace's lone tiled column. Disable to keep the configured/default width. |

Dwindle layout options:

| Key             | Type   | Default | Description |
| --------------- | ------ | ------- | ----------- |
| `preserve_split` | bool  | `false` | Keep each split direction fixed after it is created. |

Master layout options:

| Key                      | Type   | Default  | Description                                                                 |
| ------------------------ | ------ | -------- | --------------------------------------------------------------------------- |
| `position`               | string | `"left"` | Side occupied by the master area: `"left"` or `"right"`.                    |
| `default_width_fraction` | float  | `0.55`   | Initial fraction assigned to the master area when both areas exist (0.1-0.9). |

On a vertical scrolling workspace, each column becomes a horizontal lane. Lanes
stack from top to bottom, and windows within a lane sit side by side. Existing
width vocabulary, including `default_width_fraction`, `width_presets`,
`window-cycle-width`, `window-set-width`, `window-modify-width`, and
`window-toggle-maximize`, controls the lane's extent along the scroll axis. In
other words, it controls lane height on a vertical workspace.

The packaged config sets `default_width_fraction = 0.5` so new scrolling lanes
start at half the viewport. When the option is removed, Umbriel leaves the
scroll-axis dimension unconstrained in the initial configure and retains the
logical size chosen by the client. A numeric window-rule `default_width` still
takes precedence for matching applications.

`expand_single_column` only changes the display of a column while it is the only
tiled column. Client size hints still constrain the expanded size, and the
viewport bounds take precedence over a larger minimum. The option never rewrites
the column's stored fraction, so each window's `default_width` (or the global
`default_width_fraction`) still applies the moment a second column appears.
Explicit `default_maximize` and `default_maximize_to_edges` window rules are
unaffected and win.

Directional focus and movement follow the screen: left and right operate within
a vertical lane, while up and down walk or reorder lanes along the strip.
`window-consume-left` still merges into the previous lane, which is visually
above, and `window-expel-right` creates the next lane, which is visually below.
The three-finger vertical swipe continues to switch workspaces. The
three-finger horizontal strip gesture is inert on vertical workspaces, so use
keyboard or wheel bindings to scroll the strip.
On a horizontal scrolling workspace, a three-finger horizontal swipe moves the
strip and uses release velocity when settling a column against a viewport edge.

Mod+Right-drag selects horizontal and vertical resize edges from the outer
thirds of both tiled and floating windows. Dragging from a corner region resizes
both axes. Mod+Right-click in the center region starts no resize and preserves
the window's maximize state. For tiled windows, a center click also scrolls the
focused window into view. When a tiled resize ends, the focused scrolling column
animates back into view. In the dwindle layout, only edges backed by an internal
split propose a resize, so screen-facing edges propose nothing.

When focus moves to a partially or fully hidden column, Umbriel scrolls by the
shortest distance needed to reveal it completely. A column entering from the
right aligns with the right viewport edge, and a column entering from the left
aligns with the left edge. Focus never reserves a visible sliver for the next
column.

Resizing a column recenters an underfull strip immediately.

Dragged windows become translucent so the insertion preview remains visible.
Existing window transparency still applies during the drag.
When you drag a column, the preview uses the free space beside the real column
edges. If the strip extends beyond the output, its far left and right edges
remain visible prepend and append targets, even when the corresponding end
columns are off-screen.

Dropping a window into empty space above or below a vertically resized stack
consumes that space. Existing windows retain their pixel heights, and the
dropped window fills the remainder apart from the configured inter-window gap.

In the dwindle layout, a new window splits the focused window along its
tile's longer edge and becomes the right or bottom half. With
`preserve_split = false`, split directions follow each tile's shape as
geometry changes, so closing a window can reflow the surviving splits. A
drop on a specific edge keeps that direction. Set `preserve_split = true` to
fix every split direction when it is created.

## Master layout behavior

The master layout has two areas. The master area is on the side selected by
`position`; the stack occupies the other side. Each area arranges its windows
from top to bottom. When only one area has windows, that area fills the complete
content box.

The first window becomes master. A new window also becomes master whenever the
master area is empty. Otherwise, new windows join the top of the stack. Removing
the final master window promotes the top stack window. Explicitly moving every
window out of master does not promote one, so the remaining stack stays
full-width until another window opens or is moved into master.

`window-consume-left` and `window-expel-right` preserve their visual meanings.
With `position = "left"`, consume moves a stack window into master and expel
moves a master window into the stack. With `position = "right"`, those area
roles reverse because master is visually right.

Width actions operate on the master fraction. The stack fraction is its
complement. Width actions are inert while either area is empty because the
single occupied area already fills the viewport. Tiled resizing is available
on the boundary between master and stack and on boundaries between rows in
either area.

Dragging over a master workspace previews the destination row within the
nearest area. Hint bands appear at the top, bottom, and between existing rows.
Dropping inserts the window at that row.

Layout fields can be overridden per-workspace; see
[Workspace Rules](workspaces.md#workspace-rules).
