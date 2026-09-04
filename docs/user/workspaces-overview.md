# Workspaces Overview

Configure the workspaces overview, including its navigation, shortcuts, and
appearance.

## Settings and behavior

```toml
[overview]
zoom = 0.5                     # 0.1-0.75
background_blur = true
workspace_wallpaper = true
shortcuts = true
shortcut_keys = "1234567890"
```

The wallpaper is blurred while the overview is open using the `[appearance.blur]`
parameters. Set `background_blur = false`, or disable appearance blur, to turn it
off.

Each workspace preview shows the output's background: its wallpaper and
anything else below the windows, mirrored from the background- and bottom-layer
surfaces and scaled into the preview. The real bottom layer steps aside while
the overview is open, the same way windows do behind their cards, so a surface
there appears once per workspace instead of twice at two scales. Set
`workspace_wallpaper = false` to leave the bottom layer in place and show only
the flat `colors.overview.workspace_background` fill in each preview. An output
whose clients map no background- or bottom-layer surface shows that fill either
way.

### Open and navigate

The overview shows every workspace on every output. Press `Mod+O` by default,
or use one of the [overview actions](actions.md#overview).
Opening it immediately hides every visible scratchpad. The stored windows
remain available the next time their scratchpad is shown.
Pinned windows are hidden for as long as the overview is open and do not appear
as cards. They return with their pinned state unchanged when the overview
closes.

Click a window to focus it, middle-click to close it, or drag it to another
workspace. When a click selects a window in another scrolling column, the
column reveal runs together with the closing zoom. Use the wheel or a 3-finger
swipe to move through the workspace list. Each wheel notch or swipe step moves
one workspace at a time. A 4-finger swipe opens or closes the overview.

#### Which window actions act on

No window holds the keyboard while the overview is open, so one card at a time
carries the full `colors.border.focused` color: the window a focus or close
action would act on. It sits on the current output, which is the output holding
the cursor and the one every output-changing keybind warps the cursor to. Each
other workspace row marks its own window with a fainter border, showing where
that row would land when you zoom into it. When the current workspace is empty,
no card is marked, and those actions have nothing to act on.

#### Keyboard shortcuts

Configured `[keybinds]` continue through the normal action dispatcher while the
overview is interactive, so custom Vim-style bindings and non-navigation
actions operate on the selected workspace and card without a separate overview
mapping. Direct `window-focus-left` and `window-focus-right` actions select
neighboring cards. Direct `window-focus-up` and `window-focus-down` retain their
normal layout-specific behavior. With the default horizontal strip direction,
they traverse stacked cards in the current column. Plain `Left`/`Right` keys
that reach the overview fallback invoke the horizontal focus actions, while
fallback `Up`/`Down` keys invoke `window-focus-or-workspace-up` and
`window-focus-or-workspace-down`: they select a card above or below in the
current workspace and step to the previous or next workspace row only when the
layout has no card in that direction.

Composite focus actions keep their normal local-first behavior. For example,
`window-focus-or-workspace-down` first tries a window below and then selects the
next workspace, while `window-focus-or-output-right` falls through to the
output on the right at the card edge. Overview selection never applies the
implicit cursor warp from `input.cursor.follows_focus`; an explicit
`window-focus-warp:<id>` or an output-changing action keeps its documented warp.

Configured keybinds remain active while the closing zoom runs. A focus,
workspace, or output selection made during that interval becomes the final
landing target. Repeated workspace navigation moves the filmstrip without
extending the closing zoom.

Window cards show shortcut badges while the overview is open. Press a badge
label without modifiers to focus that window and close the overview. Every card
in the visible workspace rows receives a label, including scrolling-layout
cards that are temporarily beyond an output edge. Their badges appear with the
cards when the horizontal strip moves.

Favorite keys are assigned in `shortcut_keys` order. Cards on the active
workspace receive them first, and the preferred output is assigned before other
outputs. Cards within a workspace are ordered from left to right.
Once a card receives a label, it keeps that label while the overview remains
open, including across new windows and drag-drop reordering. Expanding beyond
the available single keys can still replace the least-favorite label because it
must become a prefix for the new multi-key labels.

When there are more cards than favorite keys, the least-favorite keys
become prefixes for multi-key labels. Type those labels in sequence. `BackSpace`
removes the last character of a pending sequence. `Escape` clears a pending
sequence first; press it again to close the overview.

Set `shortcuts = false` to hide the badges and disable shortcut selection. A
plain key configured in `[keybinds]` takes precedence over an overview badge
key. `shortcut_keys` must contain at least two unique, non-space printable ASCII
characters. Letter uniqueness ignores case, while badges preserve the case
written in the configuration.

Middle-click still closes a window card, but the close is sent on button
release. Drag the middle button vertically instead to step through workspace
rows without using the keyboard; moving beyond the drag threshold suppresses
the close.

An active client drag takes precedence. Umbriel ignores requests to open the
overview until the pointer button that initiated the drag is released.

### Move windows

Dragged windows become translucent so you can see the destination beneath
them. In the dwindle layout, the preview shows the direction of the new split
before you drop the window. In the master layout, it previews the destination
row within the nearest area.

Windows can be dropped onto another workspace preview. With dynamic numbered
workspaces, dropping a window into the gap between two previews, or into the
gap above the first preview, creates a new workspace at that position and
shifts the following workspace numbers down. Umbriel keeps one empty dynamic
workspace, so other previews disappear as soon as their last window is moved
or closed, including while the overview is open. Static configured workspace
lists only accept drops onto existing previews.

### Appearance

Overview cards use the same borders, corner radius, transparency, and blur as
their windows. The live target uses `colors.border.focused` unchanged, and the
other rows' markers mix that color into `colors.border.unfocused`.
Cards also retain each surface's color description, so HDR and
extended-linear content keeps the same appearance while the overview is open.
`colors.overview.workspace_background` adds a rounded background behind each
workspace. Its alpha can produce anything from a light tint to an opaque fill,
and the mirrored background covers it when `workspace_wallpaper` is on.
Shortcut badges use `colors.overview.badge` for their label and render a subtle
keycap background from it, matching the cheatsheet key combinations. A badge is
as tall as its label's line box and never narrower than it is tall, so a
single-character label reads as a square keycap. Badge corners use
`appearance.corner_radius` scaled by `zoom`, the radius the cards around them
draw with, capped at half the badge's shorter side.

| Key                    | Type  | Default     | Description                                                                                    |
| ---------------------- | ----- | ----------- | ---------------------------------------------------------------------------------------------- |
| `zoom`                 | float | `0.5`       | Workspace scale when fully zoomed out (0.1-0.75).                                              |
| `background_blur`      | bool  | `true`      | Blur the wallpaper behind the filmstrip. Uses the `[appearance.blur]` parameters.             |
| `workspace_wallpaper`  | bool  | `true`      | Mirror the output's background- and bottom-layer surfaces inside each workspace preview.        |
| `shortcuts`             | bool   | `true`      | Show shortcut badges and accept their plain key sequences.                                      |
| `shortcut_keys`         | string | `"1234567890"` | Favorite badge keys in preference order.                                                     |

The overview's colors are configured in
[`[colors.overview]`](appearance.md#overview-colors).
