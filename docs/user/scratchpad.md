# Scratchpads

A scratchpad is a holding area for windows you want nearby without keeping them
on a workspace. Each output has its own scratchpad, and every window assigned to
that output is shown or hidden together.

Scratchpad membership and visibility are separate:

- Moving a window to a scratchpad stores it there.
- Toggling a scratchpad shows or hides its stored windows.
- Restoring a window removes it from the scratchpad and returns it to a
  workspace.

## Basic setup

The packaged config uses these bindings:

```toml
[keybinds]
"Mod+Shift+Space" = "window-move-to-scratchpad"
"Mod+Space" = "scratchpad-toggle"
"Mod+Ctrl+Space" = "window-restore-from-scratchpad"
"Mod+Tab" = "scratchpad-focus-next"
```

A typical workflow is:

1. Focus a workspace window and press `Mod+Shift+Space` to store it.
2. Press `Mod+Space` to show the stored windows.
3. Press `Mod+Tab` to cycle focus when several windows are visible.
4. Press `Mod+Ctrl+Space` to return the focused window to its workspace.

Press `Mod+Space` instead of restoring when you only want to hide the
scratchpad again.

## Actions

| Action | What it does |
|--------|--------------|
| `window-move-to-scratchpad` | Move the focused workspace window into the target scratchpad. |
| `scratchpad-toggle` | Show or hide all scratchpad windows on the target output. |
| `window-restore-from-scratchpad` | Restore the focused scratchpad window to a workspace. |
| `window-toggle-scratchpad` | Move the focused window into the target scratchpad, or restore it when it is already that scratchpad's focused window. |
| `scratchpad-focus-next` | Focus the next visible scratchpad window, wrapping at the end. |

The `scratchpad-toggle`, restore, and focus actions do nothing when their
required window is not available. In particular, restore and focus-next require
the scratchpad to be visible.

Scratchpad visibility and cycling actions never repeat while their key is held,
even if the binding does not set `repeat = false`.

## Choosing an output

Without an output suffix, an action targets the output under the pointer. If the
pointer is outside every output, Umbriel uses the first enabled output.

Add `:<output>` to target a specific output from anywhere:

```toml
[keybinds]
"Mod+0" = "scratchpad-toggle:DP-1"
"Mod+Shift+0" = "window-move-to-scratchpad:DP-1"
"Mod+Ctrl+0" = "window-restore-from-scratchpad:DP-1"
"Mod+Alt+0" = "scratchpad-focus-next:DP-1"
```

For `window-move-to-scratchpad` and `window-toggle-scratchpad`, the suffix
selects the destination scratchpad. The source is still the focused window on
the active workspace under the pointer.

Use `umbriel outputs` inside a session to find output names such as `DP-1` or
`HDMI-A-1`.

## Visibility and focus

Moving a window preserves the target scratchpad's current visibility. By
default, a window moved into a hidden scratchpad fades out and is then removed
from the scene. A window moved into a visible scratchpad remains visible.

Showing a scratchpad focuses the window that was most recently focused there.
If no window has been focused yet, Umbriel focuses the first stored window.
Hiding it returns focus to a regular workspace window.
Opening the workspaces overview immediately hides every visible scratchpad
while keeping its windows stored.

All windows assigned to one output share its visibility state. There are no
separate named scratchpads within an output.

## Restoring windows

Umbriel remembers the source output, workspace, and whether the window was
tiled or floating. Restoring the window returns it to that workspace and
restores its tiled or floating state.

If the original output no longer exists, Umbriel restores the window on the
output targeted by the action. If the original workspace no longer exists, it
uses that output's active workspace.

Fullscreen, pinned, and maximize-to-edges state are cleared when a window enters
the scratchpad and are not restored automatically. The optional
`animation.scratchpad.fullscreen` or `animation.scratchpad.maximize` setting can
apply a new state on entry.

## Moving scratchpad windows

Scratchpad windows always float. Dragging one does not restore it or tile it on
the workspace beneath it.

Dragging a scratchpad window to another output assigns it to that output's
scratchpad and makes the destination scratchpad visible. The window keeps its
size. Umbriel only repositions it when its center would otherwise be outside the
destination output's usable area.

When an output disconnects or is disabled, its scratchpad windows move to
another enabled output. A visible scratchpad remains visible after that move.
When the original output returns, its scratchpad windows return with their
output-relative positions intact.

## Appearance and window actions

Scratchpad windows use dedicated focused and unfocused border colors:

```toml
[colors.border]
scratchpad_focused = "#E5C07BFF"
scratchpad_unfocused = "#5C4A2AFF"
```

See [Appearance](appearance.md) for the complete appearance
reference.

Scratchpad show and hide transitions, backdrop dimming and blur, and optional
entry sizing are configured under [`animation.scratchpad`](animation.md#animation).

While a scratchpad window has focus, `window-toggle-floating`,
`window-toggle-pinned`, and `window-center` are inactive. Restore the window
before using those actions.

## Troubleshooting

- If toggle does nothing, the target output has no stored scratchpad windows.
- If restore or focus-next does nothing, show the target scratchpad first.
- If an action affects the wrong monitor, move the pointer to the intended
  output or add an explicit output suffix.
- If a window will not tile, pin, or center, restore it to a workspace first.
