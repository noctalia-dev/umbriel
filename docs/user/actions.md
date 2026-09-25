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
| `<workspace>[/<output>]` | Bare digits select a 1-based position, other text selects a name, and double quotes force a name; append `/output` to scope either form |
| `<window-id>` | Window id from `umbriel windows` |
| `[<window-id>]` | The same id; the bare action targets the focused window |
| `[<output>]` | Connector or monitor name. Bare `dpms-off` and `dpms-on` target every configured output |
| `[<scratchpad>]` | Scratchpad name. The bare form selects the implicit `default` scratchpad, which exists only when no named scratchpads are configured |
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
| `output-focus-next` | Focus the next output, wrapping around |
| `output-focus-previous` | Focus the previous output, wrapping around |
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
| `window-cycle-primary-extent` | Cycle the focused area's primary extent through presets |
| `window-cycle-primary-extent-back` | Cycle the primary extent presets in reverse |
| `window-cycle-secondary-extent` | Cycle the focused area's secondary extent through presets |
| `window-cycle-secondary-extent-back` | Cycle the secondary extent presets in reverse |
| `window-modify-height-down:<delta>` | Resize the focused window from its bottom edge |
| `window-modify-height-up:<delta>` | Resize the focused window from its top edge |
| `window-modify-primary-extent:<delta>` | Change the focused area's primary extent by a fraction |
| `window-modify-secondary-extent:<delta>` | Change the focused area's secondary extent by a fraction |
| `window-modify-width-left:<delta>` | Resize the focused column from its left edge |
| `window-modify-width-right:<delta>` | Resize the focused column from its right edge |
| `window-move-down` | Move the focused window down in its column |
| `window-move-or-output-down` | Move down, or the column to the output below |
| `window-move-or-output-left` | Move the column left, or to the output left |
| `window-move-or-output-right` | Move the column right, or to the output right |
| `window-move-or-output-up` | Move up, or the column to the output above |
| `window-move-or-workspace-down` | Move down, or to the next workspace at the edge |
| `window-move-or-workspace-up` | Move up, or to the previous workspace at the edge |
| `window-move-to-output-down` | Move the focused window to the output below |
| `window-move-to-output-left` | Move the focused window to the output left |
| `window-move-to-output-next` | Move the focused window to the next output |
| `window-move-to-output-previous` | Move the focused window to the previous output |
| `window-move-to-output-right` | Move the focused window to the output right |
| `window-move-to-output-up` | Move the focused window to the output above |
| `window-move-up` | Move the focused window up in its column |
| `window-set-primary-extent:<fraction>` | Set the focused area's primary extent fraction |
| `window-set-secondary-extent:<fraction>` | Set the focused area's secondary extent fraction |
| `window-swap-next` | Swap with the next window in layout order |
| `window-swap-previous` | Swap with the previous window in layout order |

## Windows

| Action | Effect |
|--------|--------|
| `window-close:[<window-id>]` | Close the focused window, or the given window |
| `window-toggle-floating:[<window-id>]` | Float or tile the focused window, or the given window |
| `window-toggle-fullscreen` | Toggle fullscreen or exit a window covering the focus |
| `window-toggle-maximize` | Toggle full width for the focused column |
| `window-toggle-maximize-to-edges` | Toggle maximize without gaps, struts, or borders |
| `window-toggle-pinned` | Pin the focused window above other windows |

## Scratchpad

Scratchpads are global named holding areas that roam between outputs.
[Scratchpads](scratchpad.md) covers their configuration, restoration rules, and
multi-output behavior.

| Action | Effect |
|--------|--------|
| `scratchpad-focus-next:[<scratchpad>]` | Focus the next visible scratchpad window |
| `scratchpad-toggle:[<scratchpad>]` | Show or hide the selected scratchpad windows |
| `window-move-to-scratchpad:[<scratchpad>]` | Move the focused window into a scratchpad |
| `window-restore-from-scratchpad:[<scratchpad>]` | Return a scratchpad window to its saved workspace |
| `window-toggle-scratchpad:[<scratchpad>]` | Move the focused window to or from a scratchpad |

## Workspaces

Selector resolution, including forced numeric names and `/output` qualifiers, is
described in [Workspace selectors](workspaces.md#workspace-selectors).

| Action | Effect |
|--------|--------|
| `column-move-to-workspace:<workspace>[/<output>]` | Move the focused column to the selected workspace |
| `column-move-to-workspace-next` | Move the focused column to the next workspace |
| `column-move-to-workspace-previous` | Move the focused column to the previous workspace |
| `window-move-to-workspace:<workspace>[/<output>]` | Move the focused window to the selected workspace |
| `window-move-to-workspace-next` | Move the focused window to the next workspace |
| `window-move-to-workspace-previous` | Move the focused window to the previous workspace |
| `window-move-to-workspace-silent:<workspace>[/<output>]` | Move the focused window to the selected workspace silently |
| `window-move-to-workspace-silent-next` | Move the focused window to the next workspace silently |
| `window-move-to-workspace-silent-previous` | Move the focused window to the previous workspace silently |
| `workspace-move-down` | Move the focused workspace down the list |
| `workspace-move-to-output-down` | Move every workspace window to the output below |
| `workspace-move-to-output-left` | Move every workspace window to the output left |
| `workspace-move-to-output-right` | Move every workspace window to the output right |
| `workspace-move-to-output-up` | Move every workspace window to the output above |
| `workspace-move-up` | Move the focused workspace up the list |
| `workspace-next` | Switch to the next workspace on this output |
| `workspace-previous` | Switch to the previous workspace on this output |
| `workspace-set-layout:<scrolling\|dwindle\|master\|toggle>` | Set the active workspace's layout mode |
| `workspace-swap-active-output-down` | Swap active workspace windows with the output below |
| `workspace-swap-active-output-left` | Swap active workspace windows with the output left |
| `workspace-swap-active-output-next` | Swap active workspace windows with the next output |
| `workspace-swap-active-output-previous` | Swap active workspace windows with the previous output |
| `workspace-swap-active-output-right` | Swap active workspace windows with the output right |
| `workspace-swap-active-output-up` | Swap active workspace windows with the output above |
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
| `shortcuts-inhibit-toggle` | Toggle shortcuts inhibition for the focused surface |
| `submap:<name>` | Enter a submap layer, or leave one with 'reset' |

## Layout differences

Column and extent actions adapt to the active layout:

| Action group | Scrolling | Dwindle | Master |
| --- | --- | --- | --- |
| Column movement | Reorders columns | Swaps neighboring tiles | Exchanges master and stack contents |
| Consume and expel | Joins or splits columns | Swaps directional neighbors | Moves between master and stack |
| Primary extent | Changes column width | Adjusts horizontal splits | Changes master fraction |
| Secondary extent | Changes a row | Adjusts vertical splits | Changes a row |
| Layout scrolling | Pans the strip | No effect | No effect |
| Master count | No effect | No effect | Moves a window between master and stack |

See [Layout](layout.md) for geometry, directions, and resizing behavior.

## Notes

- Output direction actions do not wrap. The `next` and `previous` variants do.
- Workspace `next`, `previous`, `move-up`, and `move-down` do not wrap.
- A whole-column move preserves order, proportions, and column extent.
- Moving a multi-window column into Dwindle creates separate tiles.
- Floating and pinned behavior is described in [Layout](layout.md) and
  [Scratchpads](scratchpad.md).
- An action unavailable in the active layout does nothing from a keybind and
  returns an explanatory error through `umbriel msg`.
- `spawn:` supplies an activation token so the launched application can request
  focus. Autostart commands do not receive one.
- Bare `session-quit` asks for confirmation. Use
  `session-quit:skip-confirmation` only when an immediate exit is intended.
