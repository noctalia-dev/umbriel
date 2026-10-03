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
| `<output>` | Connector or monitor name from `umbriel outputs` |
| `<window-id>` | Window id from `umbriel windows` |
| `[<window-id>]` | The same id; the bare action targets the focused window |
| `[<output>]` | Connector or monitor name. Bare `dpms-off` and `dpms-on` target every configured output |
| `[<scratchpad>]` | Scratchpad name. The bare form selects the implicit `default` scratchpad, which exists only when no named scratchpads are configured |
| `<fraction>` | `0.1` to `1.0` of the column extent, or of the usable area for a floating window |
| `<delta>` | Signed `-0.9` to `0.9`; the result clamps to `0.1` to `1.0` |
| `<scrolling\|dwindle\|master\|toggle>` | Layout mode for `workspace-set-layout`; `toggle` cycles scrolling, dwindle, master |
| `<normal\|tabbed>` | How `column-set-display` shows the rows around the focused window: stacked, or as a tab group |
| `<index>` | Tab position for `column-focus-tab`, counted from `1` within the focused tab group; negative counts back from the last tab, `-1` |
| `[skip-confirmation]` | `session-quit` only: quit without the on-screen confirmation |

Effect `set` and `cycle` split at the first `/`; everything after it is the
window id or output name. For example, `effect-screen-set:cinema/vendor/panel`
selects `cinema` on output `vendor/panel`, and `effect-window-cycle:/window-id`
cycles that window's current pool. Omit the target for the focused window or
preferred output. Cursor actions take no target. A trailing colon or slash is
invalid. `off` is accepted only by `set`.

Named `set` and successful `cycle` create a runtime override and clear
suppression. `set:off` and `toggle` suppress without forgetting the selected
member; `reset` clears the override, suppression, and history, then resolves
configuration. An unnamed cycle needs an underlying pool; an empty pool cannot
cycle. See [Effects](effects.md) for selection and owner lifetimes.

## Apps

| Action | Effect |
|--------|--------|
| `spawn:<cmd>` | Run a command with a launch activation token |

## Screencasting

Select one screen or window normally. The selected source starts sharing immediately, and the actions below can later
change it while keeping the same PipeWire stream alive. Shares containing several selected sources remain fixed.

The first set or follow action during an active share opens an Umbriel confirmation panel. Enter, or repeat a
target-changing action, to approve it. Any other key or pointer click dismisses the panel and cancels only that pending
action. A later action asks again. Once approved, target changes are immediate until the share ends. Set
[`screencast.disable_dynamic_confirmation`](configuration.md#screencast) to `true` to skip this protection.

Changing a window stream to an output, or an output stream to a window, is supported. Follow mode lasts only for the
active portal session. Be careful with window following because focusing a private window immediately shares it.

| Action | Effect |
|--------|--------|
| `screencast-clear` | Pause the screencast and stop following |
| `screencast-follow-output` | Follow the focused output |
| `screencast-follow-stop` | Stop following and keep the current target |
| `screencast-follow-window` | Follow the focused window |
| `screencast-set-output:[<output>]` | Share the focused output, or the selected output |
| `screencast-set-window:[<window-id>]` | Share the focused window, or the selected window |

## Focus

| Action | Effect |
|--------|--------|
| `column-focus-first` | Focus the first column in the workspace |
| `column-focus-last` | Focus the last column in the workspace |
| `column-focus-tab:<index>` | Focus a tab of the focused tab group by its position |
| `column-focus-tab-next` | Focus the next tab in the focused tab group |
| `column-focus-tab-previous` | Focus the previous tab in the focused tab group |
| `output-focus-down` | Focus the output below |
| `output-focus-left` | Focus the output to the left |
| `output-focus-next` | Focus the next output, wrapping around |
| `output-focus-previous` | Focus the previous output, wrapping around |
| `output-focus-right` | Focus the output to the right |
| `output-focus-up` | Focus the output above |
| `window-focus:<window-id>` | Focus a window, revealing it from a hidden scratchpad |
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
| `window-focus-warp:<window-id>` | Focus or reveal a window and warp the cursor to it |
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
| `window-consume-from-left` | Pull the left column's window into the focused column |
| `window-consume-from-right` | Pull the right column's window into the focused column |
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
| `window-move-left` | Move the focused window left |
| `window-move-or-output-down` | Move down, or the column to the output below |
| `window-move-or-output-left` | Move the column left, or to the output left |
| `window-move-or-output-right` | Move the column right, or to the output right |
| `window-move-or-output-up` | Move up, or the column to the output above |
| `window-move-or-workspace-down` | Move down, or to the next workspace at the edge |
| `window-move-or-workspace-up` | Move up, or to the previous workspace at the edge |
| `window-move-right` | Move the focused window right |
| `window-move-to-output-down` | Move the focused window to the output below |
| `window-move-to-output-left` | Move the focused window to the output left |
| `window-move-to-output-next` | Move the focused window to the next output |
| `window-move-to-output-previous` | Move the focused window to the previous output |
| `window-move-to-output-right` | Move the focused window to the output right |
| `window-move-to-output-up` | Move the focused window to the output above |
| `window-move-up` | Move the focused window up in its column |
| `window-set-primary-extent:<fraction>` | Set the focused area's primary extent fraction |
| `window-set-secondary-extent:<fraction>` | Set the focused area's secondary extent fraction |
| `window-swap-down` | Swap with the window below |
| `window-swap-left` | Swap with the window to the left |
| `window-swap-next` | Swap with the next window in layout order |
| `window-swap-previous` | Swap with the previous window in layout order |
| `window-swap-right` | Swap with the window to the right |
| `window-swap-up` | Swap with the window above |

## Windows

| Action | Effect |
|--------|--------|
| `column-hide-tab-bar` | Hide the focused tab group's bar; its tabs take the space |
| `column-move-tab-next` | Move the focused tab one place later among its tabs |
| `column-move-tab-previous` | Move the focused tab one place earlier among its tabs |
| `column-set-display:<normal\|tabbed>` | Show the rows around the focused window as tabs or stacked |
| `column-show-tab-bar` | Show the focused tab group's bar |
| `column-toggle-tab-bar` | Show or hide the focused tab group's bar |
| `column-toggle-tabbed` | Tab the rows around the focused window, or stack its tabs |
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
| `effect-border-cycle:[<pool>][/<window-id>]` | Cycle the current or named border pool |
| `effect-border-reset:[<window-id>]` | Clear runtime state and resolve configuration |
| `effect-border-set:<name>[/<window-id>]` | Select a border preset/pool, or suppress with off |
| `effect-border-toggle:[<window-id>]` | Toggle border-slot suppression |
| `effect-cursor-cycle:[<pool>]` | Cycle the current or named cursor pool |
| `effect-cursor-reset` | Clear runtime state and resolve configuration |
| `effect-cursor-set:<name>` | Select a cursor preset/pool, or suppress with off |
| `effect-cursor-toggle` | Toggle cursor-slot suppression |
| `effect-screen-cycle:[<pool>][/<output>]` | Cycle the current or named screen pool |
| `effect-screen-reset:[<output>]` | Clear runtime state and resolve configuration |
| `effect-screen-set:<name>[/<output>]` | Select a screen preset/pool, or suppress with off |
| `effect-screen-toggle:[<output>]` | Toggle screen-slot suppression |
| `effect-window-cycle:[<pool>][/<window-id>]` | Cycle the current or named window pool |
| `effect-window-reset:[<window-id>]` | Clear runtime state and resolve configuration |
| `effect-window-set:<name>[/<window-id>]` | Select a window preset/pool, or suppress with off |
| `effect-window-toggle:[<window-id>]` | Toggle window-slot suppression |
| `keyboard-layout-next` | Switch one keyboard to its next configured layout |
| `output-disable:<output>` | Remove an output from the desktop |
| `output-enable:<output>` | Add an output to the desktop |
| `output-toggle:<output>` | Add or remove an output from the desktop |
| `session-quit:[skip-confirmation]` | Quit the session, confirming first unless told to skip |
| `shortcuts-inhibit-toggle` | Toggle shortcuts inhibition for the focused surface |
| `submap:<name>` | Enter a submap layer, or leave one with 'reset' |

## Layout differences

Column and extent actions adapt to the active layout:

| Action group | Scrolling | Dwindle | Master |
| --- | --- | --- | --- |
| Column movement | Reorders columns | Same as window left/right | Exchanges master and stack contents |
| Window left/right | Expels the window or stacks it into the neighboring column; moves it along its lane in a vertical strip | Moves the window into the neighboring tile | Moves between master and stack |
| Consume and expel | Joins or splits columns | Swaps directional neighbors | Moves between master and stack |
| Primary extent | Changes column width | Adjusts horizontal splits | Changes master fraction |
| Secondary extent | Changes a row | Adjusts vertical splits | Changes a row |
| Layout scrolling | Pans the strip | No effect | No effect |
| Master count | No effect | No effect | Moves a window between master and stack |
| Tabs | Tabs the focused column | Refused | Tabs the focused master or stack area |

See [Layout](layout.md) for geometry, directions, and resizing behavior.

## Notes

- Output direction actions do not wrap. The `next` and `previous` variants do.
- Workspace `next`, `previous`, `move-up`, and `move-down` do not wrap.
- A whole-column move preserves order, proportions, and column extent.
  Between scrolling workspaces, it also preserves tab groups, their selected
  tabs, and individual tab-bar visibility overrides.
- Moving a multi-window column into Dwindle creates separate tiles.
- `column-focus-tab-next` and `column-focus-tab-previous` wrap unless
  `layout.tabs.wrap_focus` is off. In the scrolling layout a tab group is one
  row of its column: `window-focus-up` and `window-focus-down` step over it, and
  `window-move-up` and `window-move-down` take a tab out of it or a window into
  it. In a tabbed master area they step through its tabs without wrapping.
- Floating and pinned behavior is described in [Layout](layout.md) and
  [Scratchpads](scratchpad.md).
- An action unavailable in the active layout does nothing from a keybind and
  returns an explanatory error through `umbriel msg`.
- `spawn:` supplies an activation token and records the pointer-preferred
  output and its active workspace. The launched application's first window opens there even if you
  switch workspaces while it starts. Opening on a workspace you left is
  silent, and explicit `default_output`, `default_workspace`, and
  `default_scratchpad` window rules take precedence. `focus_on_activate`
  controls whether an activation request focuses the window, not the recorded
  launch destination. Autostart commands do not receive a token or recorded
  placement.
- Bare `session-quit` asks for confirmation. Use
  `session-quit:skip-confirmation` only when an immediate exit is intended.
