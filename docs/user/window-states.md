# Window sizing and states

## Extent actions

Primary and secondary extent actions resize along these axes:

- Scrolling: primary changes column extent; secondary changes a row.
- Dwindle: primary adjusts horizontal splits; secondary adjusts vertical splits.
- Master: primary changes the master fraction; secondary changes a row.
- Floating: primary changes width; secondary changes height.

The `window-set-*` actions assign an exact fraction. `window-modify-*` changes it by a signed amount, and `window-cycle-*` walks the configured presets.

## Wide columns

A scrolling column may span more than one output: extent fractions up to `4.0` fill `fraction × (viewport + gap) - gap` pixels, so `2.0` covers two viewports with the gap between them. Values above `1.0` apply only to the scrolling layout: the other layouts clamp to one viewport, and floating extent cycles skip presets they cannot realize. `window-set-*` accepts the wider range and the layout clamps what it cannot show.

A wide column scrolls like a strip longer than the output: `layout-scroll-left`, `layout-scroll-right`, and `layout-scroll-drag` pan through it, a strip swipe rests at either of its edges, and refocusing the column keeps the pan in place instead of snapping back. Focus arriving from a neighbor shows that side's edge. Interactive resizing grows a column up to the same `4.0` ceiling, but the pointer cannot leave the output, so growth stops when the cursor reaches its edge; use the extent actions, or drag, scroll, then drag again to grow further. A client-chosen width, including a client's minimum size, still fits one viewport; wider extents come from the keys above, window rules, and resize.

## Client minimum sizes

Applications may enforce a minimum size larger than their assigned tile. Umbriel clips oversized content rather than allowing it to cover neighboring tiles. Application-specific minimums must be disabled in that application's settings when smaller tiles are required.

## Floating windows

For floating windows, extent fractions use the output's usable area and respect the application's minimum and maximum size hints. Resizing a maximized floating window leaves maximization and keeps the new size. Extent actions do nothing while the window is fullscreen.

`window-move-left`, `window-move-right`, `window-move-up`, and `window-move-down` move a floating window to that edge of the output's usable area and keep its position on the other axis. The edge sits inside the layout struts, `gap`, and borders, so the window lines up with tiles at that edge. At the edge already, the `-or-output` and `-or-workspace` variants send the window to the neighboring output or workspace.

Focusing a floating window reveals the workspace's floating windows above fullscreen without taking the fullscreen window out of fullscreen. Clicking or explicitly focusing the fullscreen window covers independent floats again. With focus follows mouse enabled, hovering the fullscreen window transfers keyboard focus while leaving the floats visible; clicking it covers them.

An independent floating window does not take focus by default when it opens over fullscreen. Set `default_focused = true` in a [window rule](window-rules-placement.md#maximize-fullscreen-and-focus) to focus and reveal it automatically. Parented dialogs and application activation requests retain their usual focus behavior.

## Dialogs

Parented dialogs stay above their parent and normally open centered over its visible area.

A modal dialog is attached to its parent: one that says so through xdg-dialog-v1, or one another application parented through xdg-foreign, the way a portal file chooser is. It hangs on the parent's frame, centered over it and kept on screen, so it goes wherever the parent goes: another workspace or output, an unplugged output's refuge, the scratchpad, pinned, fullscreen, a scrolled or moved column, a drag, or an overview drop. In the overview it shows over its parent's card instead of on a card of its own.

While it is attached, the dialog holds its parent's input: focusing or clicking the parent focuses the dialog, and the parent is shaded by `[animation.dialogs]` `dim` (see [Animation](animation.md#event-tables)). Actions aimed at the dialog that place a window, such as moving it to a workspace, floating, pinning, or the scratchpad, act on the parent; closing and killing still reach the dialog. A Mod-drag on the dialog drags the parent. Window rules that place a window (`default_workspace`, `default_output`, `default_scratchpad`, `default_pinned`, `default_position`, `default_floating`, and the size and maximize defaults) do not apply to an attached dialog.

The dialog leaves when it stops being modal or closes, and becomes an ordinary floating window if its parent closes first. Set `[general] attach_modal_dialogs = false` to keep every dialog an ordinary window, or use `attach_to_parent = false` in a [window rule](window-rules-placement.md#dialogs) for single applications. Either takes effect on reload.

## Minimize

Only floating windows minimize: `window-minimize`, the minimize button of Umbriel's [title bar](title-bars.md) or of the application, or a taskbar can hide one. Tiled, pinned, fullscreen, and scratchpad windows do not minimize, and applications and the title bar offer minimize only while the window floats. Tiling a minimized window, for example by switching its workspace to a tiling layout, shows it again.

A minimized window keeps its workspace, position, size, and stacking. It stays in taskbars and in `umbriel windows`, where it reports `minimized` (see [IPC](ipc.md)). When the focused window is minimized, focus moves to another window on the workspace. Focus cycling, directional focus, `window-focus-last`, and the overview skip minimized windows.

A minimized window comes back when:

- a taskbar activates or unminimizes it,
- the application activates itself with a valid activation token, for example when it is launched again or a notification is clicked,
- `window-focus` or `window-focus-warp` names it,
- a modal dialog attaches to it.

Without a taskbar, `window-restore-minimized` restores the most recently minimized window of the active workspace; repeat it to restore the one before. For a specific window, find its ID with `umbriel windows --json` and run `umbriel msg window-focus <window-id>`.

`workspace-toggle-show-desktop` minimizes every floating window of the active workspace. Once none is left to hide, the next toggle restores every minimized window of that workspace, including ones minimized another way, and returns focus to the window that had it.

## Maximize and fullscreen

`window-toggle-fullscreen` fills the complete output, ignoring struts and panel exclusive zones. A floating window keeps its normal size and position when moved through a tiled workspace while fullscreen and back to a stacking workspace. Leaving fullscreen then restores that original floating box. Entering fullscreen during a resize still restores the requested size afterward.

Visible floating windows above fullscreen require composition on that output. Once fullscreen covers them again, direct scanout can resume if the fullscreen buffer meets the other scanout requirements.

`window-toggle-maximize` fills the layout area. Tiled columns keep struts and gaps; floating windows fill the output's usable area inside the same margin, and follow it when the gap, borders, struts, or usable area change.

A maximized window stays maximized when you float it and then unfloat it. Pressing the maximize binding after that round trip restores the column's extent from before the initial maximize. Repeated float/unfloat cycles preserve that restore extent until you unmaximize manually.

Unmaximizing while floating restores the remembered normal floating size and position. If the window has never floated, its restore size comes from its ordinary tile before maximization.

`window-toggle-maximize-to-edges` removes layout struts, gaps, and borders while leaving panel exclusive zones visible. An enabled title bar stays at the top of that usable area and reserves its height above the content, keeping the restore button reachable. This applies to tiled and floating windows.

In Master and Dwindle, an existing tiled window leaves maximize-to-edges when another window joins the tiled layout, restoring their normal tile sizes. Floating windows keep their maximized state.

With `client_maximize = "edges"`, a tiled or stacking window's own maximize button (or titlebar double-click) behaves like `window-toggle-maximize-to-edges` instead of `window-toggle-maximize`. In stacking, `"column"` keeps the normal floating maximize inside the layout margins. Floating windows above tiling layouts keep their normal floating maximize behavior. The `window-toggle-maximize` action keeps its normal layout behavior, so bind `window-toggle-maximize-to-edges` to get the edges from a keybind. Windows without their own maximize control, such as most terminals, never send that request. Unmaximizing runs that toggle in reverse: the edges are left first, and a window whose column is also at full width leaves the column on the next unmaximize. A maximize restored at open under `honor_restored_maximize` follows the same policy in tiled and stacking layouts. On a `default_maximize` tile the edges sit above the rule's full-width column, so unmaximizing it takes two steps; without a client request the rule opens the column only.
