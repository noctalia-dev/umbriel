# Scratchpads

A scratchpad stores windows outside normal workspaces so they can be shown or hidden quickly. Its windows float by default, or [tile with a layout](#layout), and appear together.

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

## One key to show and hide

`scratchpad-window-cycle` shows and hides one window at a time on a single key:

```toml
[keybinds]
"Mod+S" = "scratchpad-window-cycle"
"Mod+Ctrl+S" = "scratchpad-window-cycle-previous"
```

- If a scratchpad window has focus, it hides the scratchpad.
- If the scratchpad is shown but another window has focus, such as a pinned window or one on another output, it focuses the scratchpad, moving it to this output if needed.
- Otherwise it shows the window after the one you used last, or before it with `scratchpad-window-cycle-previous`. If the scratchpad was dismissed some other way, such as by focusing a window behind it, it shows the same window again.

With windows A, B and C, repeated presses show A, hide, show B, hide, show C, hide, and start again at A. The order is the same as `scratchpad-window-show-next`, so both can be bound together.

## Named scratchpads

Define names when you want several independent scratchpads:

```toml
[[scratchpad]]
name = "terminal"

[[scratchpad]]
name = "music"

[keybinds]
"Mod+Shift+Space" = "window-toggle-scratchpad terminal"
"Mod+Space" = "scratchpad-toggle terminal"
"Mod+Shift+M" = "window-toggle-scratchpad music"
"Mod+M" = "scratchpad-toggle music"
```

Defining any named scratchpad removes the implicit `default`. Every scratchpad action must then include a configured name. Names must be unique and `default` is reserved.

Removing a scratchpad on reload restores its windows to their saved destinations.

## Layout

A scratchpad arranges its windows like a workspace of its own, outside every monitor's workspace list. Its windows float by default, as in the `stacking` layout. A named scratchpad accepts the `layout` keys of a [`[[workspace]]` rule](workspaces.md#available-fields), so it can tile its windows instead:

```toml
[[scratchpad]]
name = "terminals"
layout.mode = "dwindle"
layout.struts = { left = 200, right = 200, top = 100, bottom = 100 }
```

Without `layout.mode` the scratchpad uses `stacking`, not the global `[layout]` mode; its other `layout` keys default to the global `[layout]` values. The implicit `default` scratchpad always uses `stacking`. Tiles fill the usable area of the scratchpad's monitor, less its struts.

Window and layout actions on a focused scratchpad window act on the scratchpad's layout, as they would on a workspace. `workspace-set-layout` changes the layout of a focused scratchpad until the next configuration reload. A window moved into a tiling scratchpad tiles unless it was explicitly floating.

While the scratchpad [shows one window at a time](#one-window-at-a-time), that window keeps its place in the layout.

## Assigning new windows automatically

### Launching into the open scratchpad

Opening a scratchpad makes it the launch destination on that output, even when it is empty. Its backdrop appears immediately; an empty pad has no window receiving keyboard input. Window-relative bindings leave the workspace windows behind it untouched. Applications opened through `spawn` join that pad without a window rule. Launchers that pass a trusted XDG activation token can also carry this destination to a new window.

The destination is recorded at launch. Hiding the pad before the application maps leaves its new window hidden in that pad. Replacing it with another pad does not redirect the pending launch or give its arriving window focus.

Closing the last member leaves the empty pad open. Toggle it to dismiss it, or set [`hide_when_empty`](#hiding-an-emptied-scratchpad) to hide it automatically. Restoring the last member to its workspace hides the pad. A pad showing one window at a time also hides when that selected window closes.

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

### Hiding an emptied scratchpad

Set `hide_when_empty` on a named scratchpad to hide it when its last window closes or hides itself, so the next toggle shows it and runs `spawn_when_empty` in one press:

```toml
[[scratchpad]]
name = "terminal"
spawn_when_empty = "foot"
hide_when_empty = true
```

Only losing the last window hides the pad. An empty pad you open explicitly stays open while its application starts, and closing one of several windows leaves the pad open. A hidden pad stays hidden. If the closing window had keyboard focus, focus returns to the workspace. A window that later joins the pad, such as one from a pending launch, arrives hidden. The setting does not need `spawn_when_empty`.

## Actions

| Action                                            | What it does                                                                                                                               |
| ------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| `window-move-to-scratchpad [<scratchpad>]`        | Store the focused workspace window.                                                                                                        |
| `scratchpad-toggle [<scratchpad>]`                | Show or hide the selected scratchpad, including when empty. Opening an empty pad also runs its `spawn_when_empty` command when configured. |
| `window-restore-from-scratchpad [<scratchpad>]`   | Restore one remembered window.                                                                                                             |
| `window-toggle-scratchpad [<scratchpad>]`         | Store the focused window or restore it when already selected.                                                                              |
| `scratchpad-focus-next [<scratchpad>]`            | Focus the next visible member.                                                                                                             |
| `scratchpad-window-show-next [<scratchpad>]`      | Show only the next member, hiding the one on show.                                                                                         |
| `scratchpad-window-show-previous [<scratchpad>]`  | Show only the previous member, hiding the one on show.                                                                                     |
| `scratchpad-window-cycle [<scratchpad>]`          | Show only the next member, or hide the scratchpad when it has focus.                                                                       |
| `scratchpad-window-cycle-previous [<scratchpad>]` | Show only the previous member, or hide the scratchpad when it has focus.                                                                   |

The argument is optional only for the implicit `default` scratchpad. Restore and focus-next require the scratchpad to be visible.

`window-focus <window-id>` summons a matching hidden scratchpad window before focusing it. `window-focus-warp <window-id>` also moves the cursor to it.

## Choosing an output

Scratchpads are global rather than owned by one output. The pointer selects the output where a hidden scratchpad appears.

Toggling a scratchpad already visible on another output moves it to the invoking output. Toggling it again on the same output hides it. Only one scratchpad can be visible on an output, but different outputs can show different scratchpads.

### Proportional sizing across monitors

Scratchpad windows keep their logical-pixel size by default. Enable `proportional_size` on a named scratchpad to preserve each window's width and height as proportions of the usable area when it moves to another monitor:

```toml
[[scratchpad]]
name = "terminal"
proportional_size = true
```

A window occupying 88% of one monitor's usable width occupies approximately 88% on the next, subject to the application's size limits. Manual resizing establishes the proportions used for the next monitor transfer; the initial window size rule is not reapplied. Showing, hiding, or moving the window within the same monitor does not resize it. Changes to that monitor's scale, mode, or panels do not proportionally resize it either.

Fullscreen and maximized windows continue to fit their monitor, and their saved normal size follows the proportional setting. Restoring a window or moving it to a workspace ends proportional sizing and restores its saved tiled or floating state. Floating windows keep their current size; tiled windows follow the destination layout.

## Visibility and focus

Showing a populated scratchpad focuses its most recently focused member. Hiding it returns focus to a workspace window. Opening the workspace overview hides every visible scratchpad without removing its stored windows. While the overview is open, including its closing animation, scratchpad actions and window rules do not show a scratchpad.

A visible scratchpad owns its output's input. The pointer reaches only its windows, pinned windows, and layer surfaces such as panels; the workspace windows behind it get neither clicks, hover, nor Mod-drags. Clicking the backdrop hides the scratchpad and focuses the workspace. Any other focus change to a workspace window there also hides it, such as `window-focus` or an application activating, and so does switching or swiping to another workspace on that output unless the scratchpad is sticky. With focus following the mouse, moving the pointer onto the scratchpad's output focuses the scratchpad.

Set `modal = false` on a named scratchpad to keep it up while you use the windows behind it. Clicks and focus reach those windows, the scratchpad stays shown and draws no backdrop, and switching workspace still hides it unless it is sticky.

Set `sticky = true` on a named scratchpad to keep it up when you switch or swipe to another workspace on its output. It keeps focus and stays over the new workspace until you hide it.

```toml
[[scratchpad]]
name = "terminal"
modal = false
sticky = true
```

The implicit `default` scratchpad is always modal and never sticky.

A dialog parented to a visible scratchpad window joins that scratchpad unless a window rule explicitly chooses another destination.

Backdrop dim and blur affect only the output showing a modal scratchpad. A member that closes while its scratchpad is shown fades like any other window, over the pad's backdrop rather than under it, and a scratchpad hidden mid-close takes the closing window with it; a non-modal pad has no backdrop, but its closing member still follows the pad's fade. Hiding a pad fades the dim and blur out with it rather than cutting them at the first frame, and each pad owns its own fade, so showing another pad while one is hiding leaves the hiding pad's closing window hidden.

A shown scratchpad sits above workspace windows and below panels. When a fullscreen window is on its output, or one of its own windows is fullscreen, the scratchpad and its backdrop draw above fullscreen windows instead, until it has faded out. This applies to every shown scratchpad at once, so a scratchpad shown on another output at the same time also draws above that output's panels. Pinned windows, overlay-layer surfaces such as notifications, and the lock screen stay above it.

Showing a scratchpad never moves a floating window it holds: the window stays where it was left, even hanging off an output edge as far as a floating window may. A pad that moves to another output carries its floating windows' positions proportionally, and a window moved into a pad that shows on another output joins that output.

## Restoring windows

Each member remembers its source output, workspace, and tiled or floating state. If that output or workspace no longer exists, Umbriel restores the window to the scratchpad's current output and active workspace.

Fullscreen, pinned, and maximize-to-edges state are cleared when a window enters a scratchpad. In a floating scratchpad the window floats at its current size. A window that opens into a scratchpad through `default_scratchpad` takes its size, position, fullscreen, and maximize-to-edges state from its [window rules](window-rules-placement.md#open-in-a-scratchpad).

## Moving scratchpad windows

Workspace move bindings remove the focused window from its scratchpad and send it to the selected workspace, restoring its saved tiled or floating state. Use `window-move-to-workspace <workspace> [--output <output>]` to follow the window, or `window-move-to-workspace-silent <workspace> [--output <output>]` to keep the current workspace active. The next and previous variants work too, using the active workspace on the scratchpad's output as their starting point. Other members stay in the scratchpad. Moving its last member out hides the scratchpad.

Dragging a scratchpad window does not restore it. Dragging one member to another output moves the whole scratchpad there while preserving the other members' relative positions.

If its output disconnects, the scratchpad temporarily moves to another enabled output and returns when the original output becomes available, unless the user has deliberately moved it elsewhere.

## Appearance and window actions

Scratchpad windows use the regular border colors unless a [window rule](window-rules-appearance.md#borders-that-show-window-state) matches them:

```toml
[[window_rule]]
match.is_scratchpad = true
border_color_focused = "#E5C07BFF"
border_color_unfocused = "#5C4A2AFF"
```

Show and hide transitions and backdrop dimming and blur are configured under [`animation.scratchpad`](animation.md#animation).

Window state, size, and [layout](#layout) actions work on a focused scratchpad window. `window-toggle-pinned` takes the window out of the scratchpad and pins it on the workspace under the scratchpad; unpinning it later returns it to its saved tiled or floating state there. Minimizing and moving a single window to another monitor do not work: a scratchpad moves between monitors as a whole.

## Troubleshooting

- If an empty pad is hard to distinguish, check its backdrop dim and blur settings.
- If restore or focus-next does nothing, show the scratchpad first.
- If a named action does nothing, check its suffix against configured names.
- If it appears on the wrong monitor, move the pointer before showing it.
- Restore a window before minimizing it.
