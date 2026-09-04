# Actions

Actions are the compositor's verbs. Bind one under `[keybinds]`, attach it to a
[hot corner](keybinds.md#hot-corners), or run it with `umbriel msg <action>`.
`umbriel msg --help` prints this same list, with the same wording and the same
grouping. See [Keybinds](keybinds.md) for chord syntax and [IPC](ipc.md) for the
socket behind `umbriel msg`, including the event stream that reports what an
action changed.

## Argument forms

An action takes at most one argument, appended after a colon. `<angle>` forms
are required, `[bracket]` forms are optional.

| Form | Meaning |
|------|---------|
| `<cmd>` | Command line, run through the shell: `spawn:kitty` |
| `<name>` | Submap to enter; `submap:reset` leaves one level |
| `<workspace>[/<output>]` | Workspace name or 1-based position, optionally qualified by output: `workspace-switch:CHAT/HDMI-A-1` |
| `<window-id>` | Window id from `umbriel windows` |
| `[<window-id>]` | The same id; the bare action targets the focused window |
| `[<output>]` | Connector or monitor name. Bare scratchpad actions target the output under the pointer; bare `dpms-off` and `dpms-on` target every configured output |
| `<fraction>` | `0.1` to `1.0` of the column extent, or of the usable area for a floating window |
| `<delta>` | Signed `-0.9` to `0.9`; the result clamps to `0.1` to `1.0` |
| `<scrolling\|dwindle\|master\|toggle>` | Layout mode for `workspace-set-layout`; `toggle` cycles scrolling, dwindle, master |
| `[skip-confirmation]` | `session-quit` only: quit without the on-screen confirmation |

## Apps

| Action | Effect |
|--------|--------|
| `spawn:<cmd>` | Run a command with a launch activation token |

## Focus

| Action | Effect |
|--------|--------|
| `column-focus-first` | Focus the first column in the workspace |
| `column-focus-last` | Focus the last column in the workspace |
| `output-focus-down` | Focus the output below |
| `output-focus-left` | Focus the output to the left |
| `output-focus-right` | Focus the output to the right |
| `output-focus-up` | Focus the output above |
| `window-focus:<window-id>` | Focus the given window |
| `window-focus-down` | Focus the next window down in the column |
| `window-focus-last` | Focus the previously focused window |
| `window-focus-left` | Focus the window to the left |
| `window-focus-next` | Focus the next window in layout order |
| `window-focus-or-output-down` | Focus down, or the output below at the edge |
| `window-focus-or-output-left` | Focus left, or the output left at the edge |
| `window-focus-or-output-right` | Focus right, or the output right at the edge |
| `window-focus-or-output-up` | Focus up, or the output above at the edge |
| `window-focus-or-workspace-down` | Focus down, or the next workspace at the edge |
| `window-focus-or-workspace-up` | Focus up, or the previous workspace at the edge |
| `window-focus-previous` | Focus the previous window in layout order |
| `window-focus-right` | Focus the window to the right |
| `window-focus-switch-floating` | Focus the last window of the opposite floating state |
| `window-focus-up` | Focus the next window up in the column |
| `window-focus-warp:<window-id>` | Focus the given window and warp the cursor to it |
| `workspace-focus-last` | Focus the previously active workspace |

## Move & size

Sizing rules per layout live in [Sizing behavior](layout.md#sizing-behavior).

| Action | Effect |
|--------|--------|
| `column-center` | Center the focused column in the viewport |
| `column-move-left` | Move the focused column one position left |
| `column-move-right` | Move the focused column one position right |
| `column-move-to-first` | Move the focused column to the first position |
| `column-move-to-last` | Move the focused column to the last position |
| `column-move-to-output-down` | Move the focused column to the output below |
| `column-move-to-output-left` | Move the focused column to the output left |
| `column-move-to-output-right` | Move the focused column to the output right |
| `column-move-to-output-up` | Move the focused column to the output above |
| `layout-master-count-decrease` | Demote the last master window to the stack |
| `layout-master-count-increase` | Promote the first stack window to master |
| `layout-scroll-down` | Scroll the strip toward its end |
| `layout-scroll-drag` | Pan the strip while the bound button is held |
| `layout-scroll-left` | Scroll the strip toward its start |
| `layout-scroll-right` | Scroll the strip toward its end |
| `layout-scroll-up` | Scroll the strip toward its start |
| `window-center` | Center the focused floating window on its output |
| `window-consume-left` | Stack the focused window into the column left |
| `window-consume-or-expel-left` | Split the window out, or stack it into the column left |
| `window-consume-or-expel-right` | Split the window out, or stack it into the column right |
| `window-consume-right` | Stack the focused window into the column right |
| `window-cycle-height` | Cycle the focused window through the height presets |
| `window-cycle-height-back` | Cycle the height presets in reverse |
| `window-cycle-width` | Cycle the focused column through the width presets |
| `window-cycle-width-back` | Cycle the width presets in reverse |
| `window-modify-height:<delta>` | Change the focused window's height by a fraction |
| `window-modify-width:<delta>` | Change the focused column's width by a fraction |
| `window-move-down` | Move the focused window down in its column |
| `window-move-or-output-down` | Move down, or the column to the output below |
| `window-move-or-output-left` | Move the column left, or to the output left |
| `window-move-or-output-right` | Move the column right, or to the output right |
| `window-move-or-output-up` | Move up, or the column to the output above |
| `window-move-or-workspace-down` | Move down, or to the next workspace at the edge |
| `window-move-or-workspace-up` | Move up, or to the previous workspace at the edge |
| `window-move-to-output-down` | Move the focused window to the output below |
| `window-move-to-output-left` | Move the focused window to the output left |
| `window-move-to-output-right` | Move the focused window to the output right |
| `window-move-to-output-up` | Move the focused window to the output above |
| `window-move-up` | Move the focused window up in its column |
| `window-set-height:<fraction>` | Set the focused window's height fraction |
| `window-set-width:<fraction>` | Set the focused column's width fraction |
| `window-swap-next` | Swap with the next window in layout order |
| `window-swap-previous` | Swap with the previous window in layout order |

## Windows

| Action | Effect |
|--------|--------|
| `window-close:[<window-id>]` | Close the focused window, or the given window |
| `window-toggle-floating` | Float or tile the focused window |
| `window-toggle-fullscreen` | Toggle fullscreen or exit a window covering the focus |
| `window-toggle-maximize` | Toggle full width for the focused column |
| `window-toggle-maximize-to-edges` | Toggle maximize without gaps, struts, or borders |
| `window-toggle-pinned` | Pin the focused window above other windows |

## Scratchpad

Every output has its own holding area. [Scratchpads](scratchpad.md) covers the
workflow, restoration rules, and multi-output behavior.

| Action | Effect |
|--------|--------|
| `scratchpad-focus-next:[<output>]` | Focus the next visible scratchpad window |
| `scratchpad-toggle:[<output>]` | Show or hide the output's scratchpad windows |
| `window-move-to-scratchpad:[<output>]` | Move the focused window into the scratchpad |
| `window-restore-from-scratchpad:[<output>]` | Return the scratchpad window to its saved workspace |
| `window-toggle-scratchpad:[<output>]` | Move the focused window to or from the scratchpad |

## Workspaces

Selector resolution, including numeric names and `/output` qualifiers, is
described in [Workspace selectors](workspaces.md#workspace-selectors).

| Action | Effect |
|--------|--------|
| `column-move-to-workspace:<workspace>[/<output>]` | Move the focused column to the selected workspace |
| `column-move-to-workspace-next` | Move the focused column to the next workspace |
| `column-move-to-workspace-previous` | Move the focused column to the previous workspace |
| `window-move-to-workspace:<workspace>[/<output>]` | Move the focused window to the selected workspace |
| `window-move-to-workspace-next` | Move the focused window to the next workspace |
| `window-move-to-workspace-previous` | Move the focused window to the previous workspace |
| `workspace-move-down` | Move the focused workspace down the list |
| `workspace-move-to-output-down` | Move every workspace window to the output below |
| `workspace-move-to-output-left` | Move every workspace window to the output left |
| `workspace-move-to-output-right` | Move every workspace window to the output right |
| `workspace-move-to-output-up` | Move every workspace window to the output above |
| `workspace-move-up` | Move the focused workspace up the list |
| `workspace-next` | Switch to the next workspace on this output |
| `workspace-previous` | Switch to the previous workspace on this output |
| `workspace-set-layout:<scrolling\|dwindle\|master\|toggle>` | Set the active workspace's layout mode |
| `workspace-switch:<workspace>[/<output>]` | Switch to the selected workspace |

## Overview

Dragging windows between previews and creating workspaces by dropping into a gap
are described in [Overview](workspaces-overview.md).

| Action | Effect |
|--------|--------|
| `overview-close` | Close the workspace overview |
| `overview-open` | Open the workspace overview |
| `overview-toggle` | Open or close the workspace overview |

## System

| Action | Effect |
|--------|--------|
| `cheatsheet-close` | Hide the keybind cheatsheet |
| `cheatsheet-open` | Show the keybind cheatsheet |
| `cheatsheet-toggle` | Show or hide the keybind cheatsheet |
| `config-reload` | Reload the configuration file |
| `dpms-off:[<output>]` | Power off one output, or every output when bare |
| `dpms-on:[<output>]` | Power on one output, or every output when bare |
| `keyboard-layout-next` | Switch one keyboard to its next configured layout |
| `session-quit:[skip-confirmation]` | Quit the session, confirming first unless told to skip |
| `submap:<name>` | Enter a submap layer, or leave one with 'reset' |

## Layout differences

Column-scoped actions operate on the active layout's column projection,
described in [Layouts](layout.md). Where that projection changes what an action
does, this is the full list:

| Action | Scrolling | Dwindle | Master |
|--------|-----------|---------|--------|
| `column-move-left`, `column-move-right` | Reorders the focused column | Swaps the focused window with the neighboring tile | Exchanges the master and stack contents, and does nothing while either area is empty |
| `column-move-to-first`, `column-move-to-last` | Moves the column to that end of the workspace | Swaps the focused window with the first or last tile | Exchanges master and stack unless the focused area is already first or last |
| `window-consume-left`, `window-consume-right` | Stacks the window into the adjacent column | Swaps with the adjacent on-screen tile in that direction | Moves the window between the master and stack areas |
| `window-consume-or-expel-left`, `window-consume-or-expel-right` | Splits a stacked window into its own column, or stacks a lone window into the adjacent one | Swaps with the adjacent on-screen tile in that direction | Moves the window between the master and stack areas |
| `window-set-height`, `window-modify-height`, `window-cycle-height`, `window-cycle-height-back` | Sizes the row within its column | Adjusts the vertical splits containing the window | Sizes the row within the master or stack area |
| `column-center` | Centers the column in the viewport | Returns an IPC error | Returns an IPC error |
| `layout-scroll-left`, `layout-scroll-right`, `layout-scroll-up`, `layout-scroll-down`, `layout-scroll-drag` | Pans the strip | No effect | No effect |
| `layout-master-count-increase`, `layout-master-count-decrease` | No effect | No effect | Moves one window between master and stack |

Dwindle and master have no horizontal viewport, so the vertical splits and areas
absorb what scrolling would express as column geometry. On a vertical scrolling
workspace the directional actions follow their visual directions; see [Vertical
workspaces](layout.md#vertical-workspaces).

## Notes

- **Launch tokens.** `spawn:` exports a one-shot `XDG_ACTIVATION_TOKEN` and a
  matching `DESKTOP_STARTUP_ID` to the command. A single-instance application
  can pass that token to its existing window so Umbriel reveals it, including
  when the window remaps after hiding in a tray. Startup commands from
  `general.autostart` receive no launch token.
- **Quitting.** Bare `session-quit` opens an on-screen confirmation: Enter or
  the quit bind confirms, any other key or a click cancels, and a second
  `session-quit` also quits. While the session is locked it quits without the
  dialog.
- **Cursor.** With `input.cursor.follows_focus` enabled, focus navigation and
  window or column transfers between workspaces warp the cursor to the visible
  center of the focused window. This includes transfers to another output.
  Pointer-driven and automatic focus changes never move the cursor. `window-
  focus:<window-id>` stays focus-only, while `window-focus-warp:<window-id>`
  always moves it.
- **Across outputs.** Directions never wrap: with no monitor in that direction
  the action fails with an IPC error naming it ("no output to the left" and
  friends). Otherwise the cursor warps to the center of the target monitor so
  focus follows the action. With `input.cursor.follows_focus` enabled, a moved
  window or column uses its focused window's visible center instead. Output
  direction comes from output centers in logical coordinates, so fractional-
  scale rounding does not hide a neighbor.
- **Cycling.** `window-focus-next` and `window-focus-previous` walk the tiled
  windows in layout order, then the floating ones, wrapping in both directions.
  `window-swap-next` and `window-swap-previous` exchange the focused tiled
  window with its layout-order neighbor, wrap at both ends, and keep focus on
  the moved window.
- **Workspace order.** `workspace-next`, `workspace-previous`, `workspace-move-
  up`, and `workspace-move-down` never wrap: on the first workspace, the
  previous forms are silent no-ops. On a dynamic output, `workspace-next`
  reaches the trailing empty workspace, which becomes active as usual.
- **Columns as a unit.** A whole-column move preserves member order, the column
  width, its full-width restore value, and stacked row proportions. In master
  layout it transfers every member of the focused master or stack area. A column
  moved onto a dwindle workspace flattens into single-window columns. With a
  floating window focused there is no column, so the destination-moving column
  actions act like their window counterparts.
- **Floating and pinned.** `window-toggle-floating` remembers the window's
  floating size and position; the first time a window floats, Umbriel places it
  slightly below and to the right of its tiled position while keeping it on-
  screen. `window-toggle-pinned` floats the window and keeps it above fullscreen
  windows on its output, visible across workspace switches and hidden for as
  long as the overview is open. A fullscreen window cannot be pinned, and making
  a pinned window fullscreen drops the pin.
- **Unavailable actions.** When an action has no meaning in the active layout,
  its keybind does nothing and `umbriel msg` returns an error naming the
  requirement.
