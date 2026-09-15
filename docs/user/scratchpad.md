# Scratchpads

A scratchpad is a global, named holding area for windows you want nearby
without keeping them on a workspace. Its windows always float and are shown or
hidden together. A scratchpad is not owned by an output: showing or dragging it
on another output moves the whole scratchpad there.

Scratchpad membership and visibility are separate:

- Moving a window to a scratchpad stores it there.
- Toggling a scratchpad shows or hides all of its stored windows.
- Restoring a window removes it from the scratchpad and returns it to the
  output and workspace where that window was stored.

## Default scratchpad

When the config has no `[[scratchpad]]` entries, Umbriel creates one implicit
scratchpad named `default`. The packaged config uses that mode, so its actions
do not need a suffix:

```toml
[keybinds]
"Mod+Shift+Space" = "window-move-to-scratchpad"
"Mod+Space" = "scratchpad-toggle"
"Mod+Ctrl+Space" = "window-restore-from-scratchpad"
"Mod+Tab" = "scratchpad-focus-next"
```

The explicit `:default` suffix is also valid in this mode.

A typical workflow is:

1. Focus a workspace window and press `Mod+Shift+Space` to store it.
2. Press `Mod+Space` to show the stored windows.
3. Press `Mod+Tab` to cycle focus when several windows are visible.
4. Press `Mod+Ctrl+Space` to return the focused window to its saved workspace.

Press `Mod+Space` instead of restoring when you only want to hide the
scratchpad again.

## Named scratchpads

Add one `[[scratchpad]]` table for each scratchpad you want:

```toml
[[scratchpad]]
name = "terminal"

[[scratchpad]]
name = "music"

[keybinds]
"Mod+Shift+Space" = "window-toggle-scratchpad:terminal"
"Mod+Space" = "scratchpad-toggle:terminal"
"Mod+Shift+M" = "window-toggle-scratchpad:music"
"Mod+M" = "scratchpad-toggle:music"
```

Defining any named scratchpad disables the implicit `default` scratchpad. In
this mode every scratchpad action must include a configured name. Names must be
nonempty and unique, and `default` is reserved for the implicit scratchpad.

On config reload, windows in a removed scratchpad return to their individually
saved outputs and workspaces. This also happens to windows in the implicit
`default` scratchpad when the first named definition is added.

## Assigning new windows automatically

Use the opening-only `default_scratchpad` window rule to store matching windows
as soon as they open:

```toml
[[scratchpad]]
name = "terminal"

[[window_rule]]
match.app_id = "^scratchpad-terminal$"
default_scratchpad = "terminal"
```

Launch this example with `foot --app-id scratchpad-terminal`. The
window opens hidden in `terminal`, ready for `scratchpad-toggle:terminal`.
With no named definitions, use `default_scratchpad = "default"` instead.

The window remembers the output, workspace, and tiled or floating state it
would otherwise have opened with. `default_output`, `default_workspace`, and
`default_floating` therefore control where and how it returns when restored.
Opening size and position rules set its initial scratchpad geometry unless
scratchpad scale, maximize, or fullscreen overrides it.
If the scratchpad is already visible, the new window joins it without hiding
the existing members.

## Actions

| Action | What it does |
|--------|--------------|
| `window-move-to-scratchpad:[<scratchpad>]` | Move the focused workspace window into the selected scratchpad. |
| `scratchpad-toggle:[<scratchpad>]` | Show or hide all windows in the selected scratchpad. |
| `window-restore-from-scratchpad:[<scratchpad>]` | Restore the selected scratchpad's remembered window. |
| `window-toggle-scratchpad:[<scratchpad>]` | Move the focused window into the selected scratchpad, or restore it when it is already that scratchpad's focused window. |
| `scratchpad-focus-next:[<scratchpad>]` | Focus the next visible window in the selected scratchpad, wrapping at the end. |

The optional argument is only optional when the implicit `default` scratchpad
exists. In named mode it is required.

Toggle, restore, and focus actions do nothing when their required window is not
available. In particular, restore and focus-next require the selected
scratchpad to be visible.

`window-focus:<window-id>` and `window-focus-warp:<window-id>` summon a
matching hidden scratchpad window to the output under the pointer before
focusing it. The `-warp` form then moves the cursor to that window.

Scratchpad visibility and cycling actions never repeat while their key is held,
even if the binding does not set `repeat = false`.

## Choosing an output

An action suffix selects a scratchpad, not an output. The output under the
pointer is the invoking output. If the pointer is outside every output,
Umbriel uses the first enabled output.

`window-move-to-scratchpad` takes the focused window from the invoking
output's active workspace. `window-toggle-scratchpad:<scratchpad>` restores the
focused window only when it belongs to the selected scratchpad. Otherwise its
move branch takes the invoking output's focused workspace window, even when a
different scratchpad currently owns keyboard focus.

A window and its dialogs move as one. Moving either of them into a scratchpad
takes the whole family, restoring brings it back together to where the parent
came from, and a dialog that opens while its parent sits in a scratchpad joins
it there.

Toggling a hidden scratchpad shows the whole scratchpad on the invoking output.
Toggling a scratchpad that is visible on another output summons it to the
invoking output without hiding it first. Toggling it again on the same output
hides it.

Moving a workspace window into a hidden scratchpad moves that scratchpad to the
invoking output while keeping it hidden. Moving a window into a scratchpad that
is already visible adds the window where the scratchpad is currently shown.

Only one scratchpad can be visible on an output. Showing one there hides the
scratchpad that was already visible. Different scratchpads can remain visible
on different outputs at the same time.

## Visibility and focus

Moving a window preserves the selected scratchpad's visibility. A window moved
into a hidden scratchpad is removed from the scene immediately by default. It
fades out first when global animations and `animation.scratchpad` are enabled.
A window moved into a visible scratchpad remains visible.

Showing a scratchpad focuses the window that was most recently focused there.
If no window has been focused yet, Umbriel focuses the first stored window.
Hiding it returns focus to a regular workspace window. Opening the workspaces
overview immediately hides every visible scratchpad while keeping its windows
stored.

Backdrop dim and blur apply only to an output with a visible scratchpad. Hiding
or moving that scratchpad clears the old output's backdrop.

## Restoring windows

Each window remembers its own source output, workspace, and tiled or floating
state from immediately before it entered the scratchpad. Roaming the scratchpad
does not change that saved destination. Restoring a window returns it there and
restores its tiled or floating state.

If the saved output no longer exists, Umbriel restores the window on the
scratchpad's current output. If the saved workspace no longer exists, it uses
that output's active workspace.

Fullscreen, pinned, and maximize-to-edges state are cleared when a window enters
the scratchpad and are not restored automatically. The optional
`animation.scratchpad.fullscreen` or `animation.scratchpad.maximize` setting can
apply a new state on entry.

## Moving scratchpad windows

Dragging a scratchpad window does not restore it or tile it on the workspace
beneath it. Dragging one member to another output moves the whole named
scratchpad there. The dragged window keeps its drop position, while the other
members retain proportional positions within the destination output's usable
area. Starting a drag from a maximized scratchpad window first restores that
window's saved floating box.

If an enabled output changes position, mode, scale, transform, or usable area,
scratchpads assigned there retain their proportional output-local positions.
Their backdrop follows the output as well. A focused visible scratchpad also
keeps keyboard focus through the change, even when the pointer is on another
output.

When an output disconnects or is disabled, each scratchpad assigned there is
parked on another output when one remains enabled. A visible scratchpad remains
visible after that move. When the original output returns, the whole scratchpad
returns with its output-relative positions intact unless it was deliberately
moved elsewhere in the meantime.

## Appearance and window actions

Scratchpad windows use dedicated focused and unfocused border colors:

```toml
[colors.border]
scratchpad_focused = "#E5C07BFF"
scratchpad_unfocused = "#5C4A2AFF"
```

See [Appearance](appearance.md) for the complete appearance reference.

Scratchpad show and hide transitions, backdrop dimming and blur, and optional
entry sizing are configured under
[`animation.scratchpad`](animation.md#animation).

While a scratchpad window has keyboard focus, width and height actions resize
it directly using `animation.windows_move`. Maximize, maximize-to-edges,
fullscreen, and bare `window-close` also target that scratchpad window.

Actions that require a workspace layout are inactive while the scratchpad has
focus. This includes plain directional focus and movement, consume and expel,
swap, workspace or output transfer, `window-toggle-floating`,
`window-toggle-pinned`, `window-center`, and `column-center`. Composite focus
actions can still use their stated output or workspace fallback. Restore the
window before using a layout-relative action on it.

## Troubleshooting

- If toggle does nothing, the selected scratchpad has no stored windows.
- If restore or focus-next does nothing, show the selected scratchpad first.
- If a named action does nothing, check that its suffix matches a configured
  scratchpad name.
- If a scratchpad appears on the wrong monitor, move the pointer to the output
  where you want to summon it before invoking the action.
- If a window will not tile, pin, or center, restore it to a workspace first.
