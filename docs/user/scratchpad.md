# Scratchpads

A scratchpad stores windows outside normal workspaces so they can be shown or hidden quickly. Its windows float and appear together.

- Moving a window to a scratchpad stores it.
- Toggling a scratchpad shows or hides its stored windows.
- An empty scratchpad can be opened and receive newly launched windows.
- Restoring a window returns it to its saved output and workspace.

## Default scratchpad

Without `[[scratchpad]]` entries, Umbriel creates one implicit scratchpad named `default`. The packaged config binds it without a name suffix:

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

You can also press `Mod+Space` before opening any windows, then use your normal application launch binding. The new window opens directly in the scratchpad.

## One window at a time

`scratchpad-toggle` shows every stored window together. To see them one at a time instead, bind `scratchpad-window-show-next` and `scratchpad-window-show-previous`:

```toml
[keybinds]
"Mod+Alt+Space" = "scratchpad-window-show-next"
"Mod+Alt+Shift+Space" = "scratchpad-window-show-previous"
```

- On a hidden scratchpad, either action shows only its most recently focused window.
- Pressing again hides that window and shows the next or previous one, wrapping at the ends.
- When `scratchpad-toggle` is showing every window, either action keeps only the window after or before the focused one.
- `scratchpad-toggle` hides the scratchpad whichever way it was shown.
- A window moved into the scratchpad while it shows one window becomes the one on show, as does one a `default_focused` rule brings in. Closing or restoring the window on show hides the scratchpad.

Both styles work on the same scratchpad, so each binding can pick the one it wants.

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

Defining any named scratchpad removes the implicit `default`. Every scratchpad action must then include a configured name. Names must be unique and `default` is reserved.

Removing a scratchpad on reload restores its windows to their saved destinations.

## Assigning new windows automatically

### Launching into the open scratchpad

Opening a scratchpad makes it the launch destination on that output, even when it is empty. Its backdrop appears immediately; an empty pad has no window receiving keyboard input. Window-relative bindings leave the workspace windows behind it untouched. Applications opened through `spawn:` join that pad without a window rule. Launchers that pass a trusted XDG activation token can also carry this destination to a new window.

The destination is recorded at launch. Hiding the pad before the application maps leaves its new window hidden in that pad. Replacing it with another pad does not redirect the pending launch or give its arriving window focus. Focusing a workspace window or clicking the exposed workspace background makes subsequent launches use the workspace again, while the scratchpad stays visible.

Closing the last member leaves the empty pad open. Toggle it to dismiss it. Restoring the last member to its workspace hides the pad. A pad showing one window at a time also hides when that selected window closes.

Explicit output, workspace, scratchpad, or enabled pinning rules take priority over the captured scratchpad destination. Unrelated windows without a launch token keep their normal placement. Activating an existing application window does not move it into the scratchpad.

### Window rules

Use a `default_scratchpad` window rule:

```toml
[[scratchpad]]
name = "terminal"

[[window_rule]]
match.app_id = "^scratchpad-terminal$"
default_scratchpad = "terminal"
```

For example, `foot --app-id scratchpad-terminal` opens hidden in `terminal`. With no named definitions, use `default_scratchpad = "default"`.

Set `default_focused = true` in the same rule to summon the scratchpad when the window opens.

The window remembers where and how it would otherwise have opened. `default_output`, `default_workspace`, and `default_floating` control that restore destination.

## Launching an application on demand

A named scratchpad can set `spawn_when_empty` to a shell command. Toggling the scratchpad open while it holds no windows also runs that command:

```toml
[[scratchpad]]
name = "monitor"
spawn_when_empty = "foot btop"
```

The launched application's first new window joins the scratchpad through its launch token; no matching window rule is needed. The empty pad is visible while the application starts. Explicit placement rules still take priority.

Toggling while the launch is still pending hides the pad: the window still joins the scratchpad when it appears, but stays hidden until the next toggle. Reopening within 10 seconds does not launch a duplicate command. If the pad remains empty after that interval, opening it can run the command again. Once the scratchpad holds a window, toggling only shows and hides it.

The implicit `default` scratchpad has no definition and therefore no `spawn_when_empty`.

## Actions

| Action                                           | What it does                                                                                                                               |
| ------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------ |
| `window-move-to-scratchpad:[<scratchpad>]`       | Store the focused workspace window.                                                                                                        |
| `scratchpad-toggle:[<scratchpad>]`               | Show or hide the selected scratchpad, including when empty. Opening an empty pad also runs its `spawn_when_empty` command when configured. |
| `window-restore-from-scratchpad:[<scratchpad>]`  | Restore one remembered window.                                                                                                             |
| `window-toggle-scratchpad:[<scratchpad>]`        | Store the focused window or restore it when already selected.                                                                              |
| `scratchpad-focus-next:[<scratchpad>]`           | Focus the next visible member.                                                                                                             |
| `scratchpad-window-show-next:[<scratchpad>]`     | Show only the next member, hiding the one on show.                                                                                         |
| `scratchpad-window-show-previous:[<scratchpad>]` | Show only the previous member, hiding the one on show.                                                                                     |

The argument is optional only for the implicit `default` scratchpad. Restore and focus-next require the scratchpad to be visible.

`window-focus:<window-id>` summons a matching hidden scratchpad window before focusing it. `window-focus-warp:<window-id>` also moves the cursor to it.

## Choosing an output

Scratchpads are global rather than owned by one output. The pointer selects the output where a hidden scratchpad appears.

Toggling a scratchpad already visible on another output moves it to the invoking output. Toggling it again on the same output hides it. Only one scratchpad can be visible on an output, but different outputs can show different scratchpads.

## Visibility and focus

Showing a populated scratchpad focuses its most recently focused member. Hiding it returns focus to a workspace window. Opening the workspace overview hides every visible scratchpad without removing its stored windows. While the overview is open, including its closing animation, scratchpad actions and window rules do not show a scratchpad.

A dialog parented to a visible scratchpad window joins that scratchpad unless a window rule explicitly chooses another destination.

Backdrop dim and blur affect only the output showing the scratchpad.

## Restoring windows

Each member remembers its source output, workspace, and tiled or floating state. If that output or workspace no longer exists, Umbriel restores the window to the scratchpad's current output and active workspace.

Fullscreen, pinned, and maximize-to-edges state are cleared when a window enters a scratchpad, and the window floats at its current size. A window that opens into a scratchpad through `default_scratchpad` takes its size, position, fullscreen, and maximize-to-edges state from its [window rules](window-rules.md#scratchpad-placement).

## Moving scratchpad windows

Workspace move bindings remove the focused window from its scratchpad and send it to the selected workspace, restoring its saved tiled or floating state. Use `window-move-to-workspace:<workspace>[/<output>]` to follow the window, or `window-move-to-workspace-silent:<workspace>[/<output>]` to keep the current workspace active. The next and previous variants work too, using the active workspace on the scratchpad's output as their starting point. Other members stay in the scratchpad. Moving its last member out hides the scratchpad.

Dragging a scratchpad window does not restore it. Dragging one member to another output moves the whole scratchpad there while preserving the other members' relative positions.

If its output disconnects, the scratchpad temporarily moves to another enabled output and returns when the original output becomes available, unless the user has deliberately moved it elsewhere.

## Appearance and window actions

Scratchpad windows use the regular border colors unless a [window rule](window-rules.md#border-colors) matches them:

```toml
[[window_rule]]
match.is_scratchpad = true
border_color_focused = "#E5C07BFF"
border_color_unfocused = "#5C4A2AFF"
```

Show and hide transitions and backdrop dimming and blur are configured under [`animation.scratchpad`](animation.md#animation).

Normal window size, centering, fullscreen, maximize, and close actions work on a focused scratchpad window. Layout-relative actions require restoring it to a workspace first.

## Troubleshooting

- If an empty pad is hard to distinguish, check its backdrop dim and blur settings.
- If restore or focus-next does nothing, show the scratchpad first.
- If a named action does nothing, check its suffix against configured names.
- If it appears on the wrong monitor, move the pointer before showing it.
- Restore a window before using tiling, pinning, or layout-relative actions.
