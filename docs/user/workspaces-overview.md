# Workspaces Overview

Configure the workspaces overview, including its navigation, shortcuts, and
appearance.

## Settings and behavior

```toml
[overview]
zoom = 0.5                     # 0.1-0.75
background_blur = true
background_tint = "#10101430"
workspace_background = "#00000044"
shortcuts = true
shortcut_keys = "1234567890"
# badge_color = "#7AA3FFFF"
```

The wallpaper is blurred while the overview is open using the `[appearance.blur]`
parameters. Set `background_blur = false`, or disable appearance blur, to turn it
off.

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
column reveal runs together with the closing zoom. Use the wheel, arrow keys,
or a 3-finger swipe to move through the workspace list. While the overview is
open, each gesture moves one workspace at a time. A 4-finger swipe opens or
closes the overview.

#### Keyboard shortcuts

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
release. Drag the middle button horizontally to pan the scrolling workspace row
under the pointer, or vertically to step through workspace rows without using
the keyboard. Moving beyond the drag threshold locks to the dominant axis and
suppresses the close. A configured `layout-scroll-drag` mouse bind takes
precedence and pans the row along its configured axis.

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
their windows. They also retain each surface's color description, so HDR and
extended-linear content keeps the same appearance while the overview is open.
`workspace_background` adds a rounded background behind each workspace. Its
alpha can produce anything from a light tint to an opaque fill.
Shortcut badges use `colors.accent_primary` for their label and derive a subtle
keycap background from that accent, matching the cheatsheet key combinations.
Set `badge_color` to replace the badge accent and derive the background from the
replacement. Badge corners follow `appearance.corner_radius`, capped at one
quarter of the badge height so the shape remains a rounded rectangle.

| Key                    | Type  | Default     | Description                                                                                    |
| ---------------------- | ----- | ----------- | ---------------------------------------------------------------------------------------------- |
| `zoom`                 | float | `0.5`       | Workspace scale when fully zoomed out (0.1-0.75).                                              |
| `background_blur`      | bool  | `true`      | Blur the wallpaper behind the filmstrip. Uses the `[appearance.blur]` parameters.             |
| `background_tint`      | color | `#10101430` | Tint composited over the desktop background. Alpha `00` leaves it untouched; `FF` hides it.    |
| `workspace_background` | color | `#00000044` | Rounded background behind each workspace. Alpha `00` makes it invisible; `FF` makes it opaque. |
| `shortcuts`             | bool   | `true`      | Show shortcut badges and accept their plain key sequences.                                      |
| `shortcut_keys`         | string | `"1234567890"` | Favorite badge keys in preference order.                                                     |
| `badge_color`          | color  | `colors.accent_primary` | Badge label accent. The keycap background is derived from this color.                                  |
