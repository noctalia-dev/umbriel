# Scratchpads

A scratchpad stores windows outside normal workspaces so they can be shown or
hidden quickly. Its windows float and appear together.

- Moving a window to a scratchpad stores it.
- Toggling a scratchpad shows or hides its stored windows.
- Restoring a window returns it to its saved output and workspace.

## Default scratchpad

Without `[[scratchpad]]` entries, Umbriel creates one implicit scratchpad named
`default`. The packaged config binds it without a name suffix:

```toml
[keybinds]
"Mod+Shift+Space" = "window-move-to-scratchpad"
"Mod+Space" = "scratchpad-toggle"
"Mod+Ctrl+Space" = "window-restore-from-scratchpad"
"Mod+Tab" = "scratchpad-focus-next"
```

A typical workflow:

1. Focus a window and press `Mod+Shift+Space` to store it.
2. Press `Mod+Space` to show or hide the scratchpad.
3. Press `Mod+Tab` to cycle through visible members.
4. Press `Mod+Ctrl+Space` to return the focused member to its workspace.

## Named scratchpads

Define names when you want several independent scratchpads:

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

Defining any named scratchpad removes the implicit `default`. Every scratchpad
action must then include a configured name. Names must be unique and `default`
is reserved.

Removing a scratchpad on reload restores its windows to their saved
destinations.

## Assigning new windows automatically

Use a `default_scratchpad` window rule:

```toml
[[scratchpad]]
name = "terminal"

[[window_rule]]
match.app_id = "^scratchpad-terminal$"
default_scratchpad = "terminal"
```

For example, `foot --app-id scratchpad-terminal` opens hidden in `terminal`.
With no named definitions, use `default_scratchpad = "default"`.

The window remembers where and how it would otherwise have opened.
`default_output`, `default_workspace`, and `default_floating` control that
restore destination.

## Actions

| Action | What it does |
| --- | --- |
| `window-move-to-scratchpad:[<scratchpad>]` | Store the focused workspace window. |
| `scratchpad-toggle:[<scratchpad>]` | Show or hide the selected scratchpad. |
| `window-restore-from-scratchpad:[<scratchpad>]` | Restore one remembered window. |
| `window-toggle-scratchpad:[<scratchpad>]` | Store the focused window or restore it when already selected. |
| `scratchpad-focus-next:[<scratchpad>]` | Focus the next visible member. |

The argument is optional only for the implicit `default` scratchpad. Restore
and focus-next require the scratchpad to be visible.

`window-focus:<window-id>` summons a matching hidden scratchpad window before
focusing it. `window-focus-warp:<window-id>` also moves the cursor to it.

## Choosing an output

Scratchpads are global rather than owned by one output. The pointer selects the
output where a hidden scratchpad appears.

Toggling a scratchpad already visible on another output moves it to the invoking
output. Toggling it again on the same output hides it. Only one scratchpad can
be visible on an output, but different outputs can show different scratchpads.

## Visibility and focus

Showing a scratchpad focuses its most recently focused member. Hiding it returns
focus to a workspace window. Opening the workspace overview hides every visible
scratchpad without removing its stored windows.

A dialog parented to a visible scratchpad window joins that scratchpad unless a
window rule explicitly chooses another destination.

Backdrop dim and blur affect only the output showing the scratchpad.

## Restoring windows

Each member remembers its source output, workspace, and tiled or floating
state. If that output or workspace no longer exists, Umbriel restores the
window to the scratchpad's current output and active workspace.

Fullscreen, pinned, and maximize-to-edges state are cleared when a window enters
a scratchpad. Scratchpad animation settings may apply a new fullscreen,
maximized, or scaled presentation while it is stored.

## Moving scratchpad windows

Dragging a scratchpad window does not restore it. Dragging one member to another
output moves the whole scratchpad there while preserving the other members'
relative positions.

If its output disconnects, the scratchpad temporarily moves to another enabled
output and returns when the original output becomes available, unless the user
has deliberately moved it elsewhere.

## Appearance and window actions

Scratchpad windows use dedicated border colors:

```toml
[colors.border]
scratchpad_focused = "#E5C07BFF"
scratchpad_unfocused = "#5C4A2AFF"
```

Show and hide transitions, backdrop dimming and blur, and optional entry sizing
are configured under
[`animation.scratchpad`](animation.md#animation).

Normal window size, fullscreen, maximize, and close actions work on a focused
scratchpad window. Layout-relative actions require restoring it to a workspace
first.

## Troubleshooting

- If toggle does nothing, the selected scratchpad has no stored windows.
- If restore or focus-next does nothing, show the scratchpad first.
- If a named action does nothing, check its suffix against configured names.
- If it appears on the wrong monitor, move the pointer before showing it.
- Restore a window before using tiling, pinning, or layout-relative actions.
