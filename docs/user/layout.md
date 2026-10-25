# Layout

Choose one layout mode for each workspace. Configure the default globally, then use workspace rules for exceptions.

## Choose a layout

```toml
[layout]
mode = "scrolling"
```

| Mode        | Arrangement                                   | Best suited to                                              |
| ----------- | --------------------------------------------- | ----------------------------------------------------------- |
| `scrolling` | Fixed-size columns in a scrollable strip      | Keeping many windows readable without shrinking all of them |
| `dwindle`   | Each new window splits an existing tile       | Flexible recursive tiling                                   |
| `master`    | Primary windows beside a stack                | Keeping one or more main windows prominent                  |
| `floating`  | Independently positioned, overlapping windows | Moving and resizing windows freely                          |

Change the current workspace at runtime with `workspace-set-layout:<mode>`. See [Actions](actions.md#argument-forms) and [Workspace Rules](workspaces.md#workspace-rules).

## Floating layout

```toml
[layout]
mode = "floating"

[appearance.title_bar]
mode = "on"
```

Every ordinary window opens floating at its preferred size. Oversized automatic windows are reduced proportionally to fit the usable area, including title bars and margins. Client-driven size changes keep automatic placement centered until you move the window. New windows start at the center and shift diagonally when they would completely cover another window whose upper-left corner is visible. Placement includes title bars and borders. The offset clears the decorated title bar, with a minimum of 32 logical pixels, and wraps to fresh diagonals at the output boundary, including when several windows open simultaneously. Dialogs center over their parent; explicit position rules take precedence.

Enable title bars to move windows without a modifier and use their close and maximize buttons. Dragging updates the window position continuously, so maximize and restore use its current position even while the pointer is held. Snap animations start where you release the window and keep the normal restore box from before the drag. The configured `appearance.drag_opacity` still applies. Enable `[input] window_snapping = true` for half-screen drop zones on all four edges and quarter-screen drop zones in all four corners.

Directional focus uses window geometry, including overlapping windows. `window-toggle-floating` is rejected in this layout. Switching to a tiling layout re-tiles windows floated by the layout, while windows explicitly floated by a rule or before entering this layout stay floating. Moving a layout-floated window to a tiling workspace also tiles it there.

## Shared settings

```toml
[layout]
gap = 8
extent_presets = [0.333, 0.5, 0.667]
new_exits_fullscreen = []  # "tiled", "floating", "pinned", "all", or an array such as ["tiled", "floating"]
client_maximize = "column"  # or "edges"
```

| Key                    | Default               | Description                                                                                                                                                                                                                                                                  |
| ---------------------- | --------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `gap`                  | `8`                   | Gap between windows in logical pixels.                                                                                                                                                                                                                                       |
| `extent_presets`       | `[0.333, 0.5, 0.667]` | Fractions used by primary and secondary extent cycle actions, in any order; cycling steps by size. Values run `0.1` to `4.0`; above `1.0` they apply only to scrolling columns (see [Wide columns](#wide-columns)).                                                          |
| `new_exits_fullscreen` | `[]`                  | Kinds of arriving window that make a fullscreen window on the workspace leave fullscreen. See [Leaving fullscreen](#leaving-fullscreen).                                                                                                                                     |
| `client_maximize`      | `"column"`            | What a tiled window's own maximize button does: `"column"` fills the column like `window-toggle-maximize`, `"edges"` fills the usable area without struts, gaps, or borders like `window-toggle-maximize-to-edges`. See [Maximize and fullscreen](#maximize-and-fullscreen). |

### Leaving fullscreen

`new_exits_fullscreen` selects which kinds of window make a fullscreen window leave fullscreen when they arrive on its workspace. A window arrives when it opens there, is moved there from another workspace or output, is dropped there by drag-and-drop, or returns there from a scratchpad.

| Value        | Arriving window                                 |
| ------------ | ----------------------------------------------- |
| `"tiled"`    | A tiled window in the Dwindle or Master layout. |
| `"floating"` | A floating window that is not pinned.           |
| `"pinned"`   | A pinned window.                                |
| `"all"`      | Any window.                                     |

A string selects one kind and an array selects several. The empty array, the default, disables the behavior. In the scrolling layout a tiled window opens as a column beside the fullscreen one and the strip scrolls to it, so it never exits fullscreen.

### Struts

```toml
[layout.struts]
left = 0
right = 0
top = 0
bottom = 0
```

Positive struts reserve extra space inside layer-shell exclusive zones. Negative values let tiled windows extend beneath panels or beyond an output edge. Floating windows and popups ignore struts.

All layouts support `Mod+Right-drag` resizing. Drag from an edge to resize one axis or from a corner to resize both.

## Scrolling layout

Scrolling keeps columns at their configured extents and moves the strip through the output. A column can contain several stacked windows.

The strip is perpendicular to the output's [workspace axis](workspaces.md#workspace-axis). Vertical workspaces use a horizontal strip; horizontal workspaces use a vertical strip.

### Settings

```toml
[layout.scrolling]
default_extent_fraction = 0.5
center_underfull_strip = true
center_focused = "never"
center_on_overview_select = false
```

| Key                         | Default   | Description                                                                                                                                    |
| --------------------------- | --------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| `default_extent_fraction`   | unset     | Initial column extent from 0.1 to 4.0 (above 1.0 spans several viewports — see [Wide columns](#wide-columns)). The packaged config uses `0.5`. |
| `center_underfull_strip`    | `true`    | Center a strip narrower than the viewport.                                                                                                     |
| `center_focused`            | `"never"` | Use `"never"`, `"always"`, or `"on_overflow"` to control focus centering.                                                                      |
| `center_on_overview_select` | `false`   | Center the column picked in the overview on close, like `column-center`, whatever `center_focused` says.                                       |

### Horizontal and vertical scrolling

| Workspace axis | Scrolling arrangement                                   |
| -------------- | ------------------------------------------------------- |
| `vertical`     | Columns run left to right; windows stack top to bottom. |
| `horizontal`   | Lanes run top to bottom; windows sit left to right.     |

Primary extent actions resize a column along the strip. Secondary extent actions resize a window within its column.

A three-finger swipe along the workspace axis switches workspaces. A swipe across it scrolls the strip. Touchpad direction follows `input.touchpad.natural_scroll`.

### Scrolling behavior

When `default_extent_fraction` is unset, applications choose their initial extent. A `default_scrolling_extent` window rule overrides the fraction, and `default_scrolling_extent_px` takes highest precedence.

Set an output-specific default with:

```toml
[output.DP-1.layout.scrolling]
default_extent_fraction = 0.4
```

A workspace rule can override both global and output defaults. Reloading these settings affects new columns only.

When focus moves to a hidden column, Umbriel scrolls just far enough to reveal it. Dragged windows show an insertion preview and can be dropped into a new or existing column.

During an application drag-and-drop operation, holding the pointer near either edge of the scrolling axis reveals offscreen columns after a short delay. The strip moves faster as the pointer approaches the edge, and stops when the pointer moves away or the drag ends. A window revealed beneath a stationary pointer becomes the active drop target, so the file or other payload can be dropped without extra pointer motion.

Rearranging a tiled window by dragging it (Mod+left-click or its tab) pans the strip the same way, reaching columns beyond the current scroll extent. The trigger zone, delay, and speed are set in [Drag edge scroll](input.md#drag-edge-scroll).

With `center_focused = "on_overflow"`, a column is centered when it and the neighbor focus came from cannot share the viewport, and a width change that makes them fit puts the pair back side by side at the edge focus came from. Closing the focused column or sending it to another workspace runs the same rule with the column that took its place. Moving or resizing the focused column does too, as does going fullscreen or maximized to edges. A lone column has no such pair, so it never centers itself; `center_underfull_strip` decides where it rests.

Closing a focused column moves focus to the nearest surviving column. When that column contains stacked windows, Umbriel restores its most recently focused member instead of always selecting its first row.

With `follows_mouse = true`, closing a focused window beneath the pointer instead focuses the tiled window that occupies that position after the layout reflows. This also applies when another row in the same scrolling column expands into the stationary pointer.

## Vertical strips

With horizontal workspaces, screen directions remain literal:

- Left and right actions move within a lane.
- Up and down actions move between lanes.
- Strip scroll actions toward up or left move toward strip start.

Configurations using vertical strips usually bind wheel navigation to `window-focus-up` and `window-focus-down` instead of the default left and right actions.

## Dwindle layout

Dwindle recursively splits tiles into independently sized regions.

### Settings

```toml
[layout.dwindle]
preserve_split = false
new_toward_cursor = false
```

| Key                 | Default | Description                                                            |
| ------------------- | ------- | ---------------------------------------------------------------------- |
| `preserve_split`    | `false` | Keep each split direction fixed after creation.                        |
| `new_toward_cursor` | `false` | Place a new window on the half of the focused tile nearer the pointer. |

### Behavior

A new window splits the focused tile along its longer edge. Without `preserve_split`, split directions may adapt as geometry changes. Enable it for stable, manually shaped regions.

The new window takes the right or bottom half of the split by default. With `new_toward_cursor`, it takes the half on the pointer's side of the tile's center instead, so the pointer stays over the new window when it was over the focused tile. The focused tile is still the one that splits, and the split axis does not change. When the pointer is on another output or the overview is open, the default applies.

Directional moves (`window-move-left`, `window-move-right`, `window-move-up`, `window-move-down`, their `-or-output` and `-or-workspace` variants, and `column-move-left` and `column-move-right`, which do the same here) take the window out of its tile and split the tile it moves into. That tile splits along its longer edge, and the window takes the half nearer where it came from. The tile it left goes back to its sibling, so windows it passes through only shrink while it is inside them and keep their place in the layout. Moving toward a single neighbor that shares its split trades sides with that neighbor. To exchange two windows without changing any tile, use the `window-swap-*` actions.

`layout-toggle-split` flips the split holding the focused tile between side by side and stacked, keeping its ratio. The flipped split stays fixed, like one made by a directional drop.

Dwindle has no multi-window columns. Moving one into Dwindle places its windows as separate tiles.

## Master layout

Master places one or more primary windows in a master area and the remaining windows in a stack. The master area may sit left, right, or between two stacks.

### Settings

```toml
[layout.master]
position = "left"
default_width_fraction = 0.55
new_on_top = true
new_becomes_master = false
```

| Key                      | Default  | Description                              |
| ------------------------ | -------- | ---------------------------------------- |
| `position`               | `"left"` | Use `"left"`, `"right"`, or `"center"`.  |
| `default_width_fraction` | `0.55`   | Initial master-area fraction.            |
| `new_on_top`             | `true`   | Put new stack windows at the top.        |
| `new_becomes_master`     | `false`  | Give the master slot to each new window. |

### Behavior

The first window becomes master. Later windows join the stack unless `new_becomes_master` is enabled. Use `layout-master-count-increase` and `layout-master-count-decrease` to move windows between the two areas.

In center mode, stack windows are balanced between the left and right sides. Primary extent actions resize the master area. Secondary extent actions resize rows within an area.

## Tab groups

A tab group gives each of its windows the same box and shows one of them, the active tab. A bar along one edge of the group has a slot for every window, and the active tab's slot is highlighted. In the scrolling layout a group is one row of its column: it stacks above and below other windows, takes a row's share of the column, and resizes like any row. A column can hold several groups, and a group can hold every window of its column. In the master layout the master area and each stack are tabbed as a whole. Dwindle tiles hold one window each, so they cannot be tabbed.

`column-toggle-tabbed` (built in as `Mod+W`; bind it under `[keybinds]` when your config lists its own) tabs the focused window together with the windows stacked next to it, up to the nearest group above and below; in a column with no groups that is the whole column. On a tab it stacks its group's windows again. `column-set-display:<normal|tabbed>` does either one. The focused window becomes the active tab.

| To                                  | Do                                                                                                                                                                                                                                                                                                   |
| ----------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Switch tabs                         | Click a slot, scroll over the bar, use `column-focus-tab-next` and `column-focus-tab-previous`, or jump with `column-focus-tab:<index>`                                                                                                                                                              |
| Add a tab                           | Move a window into the group with `window-move-up` or `window-move-down`, consume it into a column whose last row is a group, pull the neighboring column's window in with `window-consume-from-left` or `window-consume-from-right` while a tab is focused, or drop it on the group's bar or middle |
| Put a window above or below a group | Move a tab out with `window-move-up` or `window-move-down`, or drop a window near the group's top or bottom edge                                                                                                                                                                                     |
| Remove a tab                        | Move it out, drag its slot out of the bar, expel it, float it, or move it away                                                                                                                                                                                                                       |
| Reorder tabs                        | Use `column-move-tab-next` and `column-move-tab-previous`, or drop a window between two slots                                                                                                                                                                                                        |
| Resize a group                      | Drag the edge between it and the window beside it, or use the secondary extent actions on any of its tabs                                                                                                                                                                                            |
| Hide or show its bar                | Use `column-toggle-tab-bar`, `column-hide-tab-bar`, or `column-show-tab-bar`                                                                                                                                                                                                                         |
| Close a tab                         | Middle-click its slot, when `middle_click_closes` is on                                                                                                                                                                                                                                              |

In the scrolling layout `window-move-up` and `window-move-down` treat a group as one row. A tab steps out of its group toward the move and keeps the group's height. A window standing alone steps into a group it meets, as its nearest tab. A group of one tab moves past its neighbour like a window. Focus moves within a column step over a group and land on its active tab. In a tabbed master area they step through its tabs instead.

Focusing a window by any means shows its tab: a keybind, a click, activation, or IPC. Moving focus into a group from outside it lands on the active tab. Hidden tabs stay mapped and sized to the group, so switching tabs is instant and never resizes a window. A hidden tab is told it is suspended, so an application that honours it, such as recent GTK and Chromium-based ones, stops drawing until its tab shows again; it still receives frame callbacks at the rate of windows on hidden workspaces, for those that do not. A hidden tab that asks for attention lights its slot with the urgent color.

A group whose bar is hidden still works: its tabs take the bar's space, and the tab actions and focus moves still switch between them. Hiding the bar of one group leaves the others alone. `visible` under [`[appearance.tab_bar]`](appearance.md#tab-bar) hides every bar a group has not shown itself.

Dropping a window onto a bar inserts it between the two slots under the pointer. Dropping it on the middle half of a group adds it where new tabs go; near the group's top or bottom it lands beside the group instead. A master area that empties starts over with `default_display` when a window next arrives.

```toml
[layout.tabs]
default_display = "normal"
new_tab_position = "end"
wrap_focus = true
scroll_switches_tabs = true
middle_click_closes = false
```

| Key                    | Default    | Description                                                                                                        |
| ---------------------- | ---------- | ------------------------------------------------------------------------------------------------------------------ |
| `default_display`      | `"normal"` | How new columns and master areas show their windows: `"normal"`, or `"tabbed"` to start each as a tab group.       |
| `new_tab_position`     | `"end"`    | Where a consumed, pulled, or dropped window joins a tab group: `"end"`, or `"after_active"` beside the active tab. |
| `wrap_focus`           | `true`     | Wrap `column-focus-tab-next` and `column-focus-tab-previous` at the ends.                                          |
| `scroll_switches_tabs` | `true`     | Step through tabs with the wheel over the bar.                                                                     |
| `middle_click_closes`  | `false`    | Close a tab with a middle click on its slot.                                                                       |

A `[[workspace]]` entry's `layout.tabs` table overrides these keys for that workspace. A window rule's `default_column_display` sets the display of the column a window opens in. See [Tab bar](appearance.md#tab-bar) for the bar's look and space.

## Sizing behavior

Primary and secondary extent actions use `layout.extent_presets`:

- Scrolling: primary changes column extent; secondary changes a row.
- Dwindle: primary adjusts horizontal splits; secondary adjusts vertical splits.
- Master: primary changes the master fraction; secondary changes a row.
- Floating: primary changes width; secondary changes height.

The `window-set-*` actions assign an exact fraction. `window-modify-*` changes it by a signed amount, and `window-cycle-*` walks the configured presets.

### Wide columns

A scrolling column may span more than one output: extent fractions up to `4.0` fill `fraction × (viewport + gap) - gap` pixels, so `2.0` covers two viewports with the gap between them. Values above `1.0` apply only to the scrolling layout — the other layouts clamp to one viewport, and floating extent cycles skip presets they cannot realize. `window-set-*` accepts the wider range and the layout clamps what it cannot show.

A wide column scrolls like a strip longer than the output: `layout-scroll-left`, `layout-scroll-right`, and `layout-scroll-drag` pan through it, a strip swipe rests at either of its edges, and refocusing the column keeps the pan in place instead of snapping back. Focus arriving from a neighbor shows that side's edge. Interactive resizing grows a column up to the same `4.0` ceiling, but the pointer cannot leave the output, so growth stops when the cursor reaches its edge — use the extent actions, or drag, scroll, then drag again to grow further. A client-chosen first width still fits one viewport; wider extents come from the keys above, window rules, and resize.

### Client minimum sizes

Applications may enforce a minimum size larger than their assigned tile. Umbriel clips oversized content rather than allowing it to cover neighboring tiles. Application-specific minimums must be disabled in that application's settings when smaller tiles are required.

### Floating windows

For floating windows, extent fractions use the output's usable area and respect the application's minimum and maximum size hints. Resizing a maximized floating window leaves maximization and keeps the new size. Extent actions do nothing while the window is fullscreen.

`window-move-left`, `window-move-right`, `window-move-up`, and `window-move-down` move a floating window to that edge of the output's usable area and keep its position on the other axis. The edge sits inside the layout struts, `gap`, and borders, so the window lines up with tiles at that edge. At the edge already, the `-or-output` and `-or-workspace` variants send the window to the neighboring output or workspace.

Focusing a floating window reveals the workspace's floating windows above fullscreen without taking the fullscreen window out of fullscreen. Clicking or explicitly focusing the fullscreen window covers independent floats again. With focus follows mouse enabled, hovering the fullscreen window transfers keyboard focus while leaving the floats visible; clicking it covers them.

An independent floating window does not take focus by default when it opens over fullscreen. Set `default_focused = true` in a [window rule](window-rules.md) to focus and reveal it automatically. Parented dialogs and application activation requests retain their usual focus behavior.

### Dialogs

Parented dialogs stay above their parent and normally open centered over its visible area.

A modal dialog is attached to its parent: one that says so through xdg-dialog-v1, or one another application parented through xdg-foreign, the way a portal file chooser is. It hangs on the parent's frame, centered over it and kept on screen, so it goes wherever the parent goes: another workspace or output, an unplugged output's refuge, the scratchpad, pinned, fullscreen, a scrolled or moved column, a drag, or an overview drop. In the overview it shows over its parent's card instead of on a card of its own.

While it is attached, the dialog holds its parent's input: focusing or clicking the parent focuses the dialog, and the parent is shaded by `[animation.dialogs]` `dim` (see [Animation](animation.md#event-tables)). Actions aimed at the dialog that place a window, such as moving it to a workspace, floating, pinning, or the scratchpad, act on the parent; closing and killing still reach the dialog. A Mod-drag on the dialog drags the parent. Window rules that place a window (`default_workspace`, `default_output`, `default_scratchpad`, `default_pinned`, `default_position`, `default_floating`, and the size and maximize defaults) do not apply to an attached dialog.

The dialog leaves when it stops being modal or closes, and becomes an ordinary floating window if its parent closes first. Set `[general] attach_modal_dialogs = false` to keep every dialog an ordinary window, or use `attach_to_parent = false` in a [window rule](window-rules.md) for single applications. Either takes effect on reload.

### Minimize

Only floating windows minimize: `window-minimize`, the minimize button of Umbriel's [title bar](appearance.md#title-bar) or of the application, or a taskbar can hide one. Tiled, pinned, fullscreen, and scratchpad windows do not minimize, and applications and the title bar offer minimize only while the window floats. Tiling a minimized window, for example by switching its workspace to a tiling layout, shows it again.

A minimized window keeps its workspace, position, size, and stacking. It stays in taskbars and in `umbriel windows`, where it reports `minimized` (see [IPC](ipc.md)). When the focused window is minimized, focus moves to another window on the workspace. Focus cycling, directional focus, `window-focus-last`, and the overview skip minimized windows.

A minimized window comes back when:

- a taskbar activates or unminimizes it,
- the application activates itself with a valid activation token, for example when it is launched again or a notification is clicked,
- `window-focus` or `window-focus-warp` names it,
- a modal dialog attaches to it.

Without a taskbar, `window-restore-minimized` restores the most recently minimized window of the active workspace; repeat it to restore the one before. For a specific window, find its ID with `umbriel windows --json` and run `umbriel msg window-focus:<window-id>`.

`workspace-toggle-show-desktop` minimizes every floating window of the active workspace. Once none is left to hide, the next toggle restores every minimized window of that workspace, including ones minimized another way, and returns focus to the window that had it.

### Maximize and fullscreen

`window-toggle-fullscreen` fills the complete output, ignoring struts and panel exclusive zones. A floating window keeps its normal size and position when moved through a tiled workspace while fullscreen and back to a floating workspace. Leaving fullscreen then restores that original floating box. Entering fullscreen during a resize still restores the requested size afterward.

Visible floating windows above fullscreen require composition on that output. Once fullscreen covers them again, direct scanout can resume if the fullscreen buffer meets the other scanout requirements.

`window-toggle-maximize` fills the layout area. Tiled columns keep struts and gaps; floating windows fill the output's usable area inside the same margin, and follow it when the gap, borders, struts, or usable area change.

A maximized window stays maximized when you float it and then unfloat it. Pressing the maximize binding after that round trip restores the column's extent from before the initial maximize. Repeated float/unfloat cycles preserve that restore extent until you unmaximize manually.

Unmaximizing while floating restores the remembered normal floating size and position. If the window has never floated, its restore size comes from its ordinary tile before maximization.

`window-toggle-maximize-to-edges` removes layout struts, gaps, and borders while leaving panel exclusive zones visible. An enabled title bar stays at the top of that usable area and reserves its height above the content, keeping the restore button reachable. This applies to tiled and floating windows.

With `client_maximize = "edges"`, a tiled window's own maximize button (or titlebar double-click) behaves like `window-toggle-maximize-to-edges` instead of `window-toggle-maximize`; floating windows keep the default behavior. The key affects only requests from the application: the `window-toggle-maximize` action still fills the column, so bind `window-toggle-maximize-to-edges` to get the edges from a keybind. Windows without their own maximize control, such as most terminals, never send that request. Unmaximizing runs that toggle in reverse: the edges are left first, and a window whose column is also at full width leaves the column on the next unmaximize. A maximize restored at open under `honor_restored_maximize` lands in the edges too. On a `default_maximize` window the edges sit above the rule's full-width column, so unmaximizing it takes two steps; without a client request the rule opens the column only.
