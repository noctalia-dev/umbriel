# Layout

This page covers scrolling and dwindle layout configuration and behavior.

## Settings and behavior

```toml
[layout]
mode = "scrolling"                  # "scrolling" or "dwindle"
gap = 8                             # 0-500
width_presets = [0.333, 0.5, 0.667]

[layout.scrolling]
direction = "horizontal"             # "horizontal" or "vertical"
default_width_fraction = 0.5         # remove to let clients choose, 0.1-1.0
center_underfull_strip = true
```

Shared layout options:

| Key             | Type        | Default               | Description                                                        |
| --------------- | ----------- | --------------------- | ------------------------------------------------------------------ |
| `mode`          | string      | `"scrolling"`         | Layout algorithm: `"scrolling"` or `"dwindle"`.                    |
| `gap`           | int         | `8`                   | Gap between windows in pixels (0-500).                             |
| `width_presets` | float array | `[0.333, 0.5, 0.667]` | Widths visited by the `window-cycle-width` action in both layouts. |

Scrolling layout options:

| Key                      | Type   | Default        | Description                                                                                                                       |
| ------------------------ | ------ | -------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| `direction`              | string | `"horizontal"` | Scroll axis: `"horizontal"` stacks columns left to right; `"vertical"` stacks lanes top to bottom.                                |
| `default_width_fraction` | float  | unset          | Initial scroll-axis extent assigned to new scrolling lanes (0.1-1.0). The packaged config sets `0.5`; when omitted, the client chooses its initial extent. |
| `center_underfull_strip` | bool   | `true`         | Center the complete strip whenever it is shorter than the viewport. Disable to align it at the start edge.                        |

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

In the dwindle layout, a new window splits an existing one along that window's
longer edge, so a landscape monitor starts side by side and a portrait monitor
starts stacked. The direction is fixed when the split is created: resizing one
boundary never reorients another split. Dropping a window on a specific edge
picks that direction explicitly instead.

Layout fields can be overridden per-workspace; see
[Workspace Rules](workspaces.md#workspace-rules).
