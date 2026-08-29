# Actions

This page lists every action that can be assigned to a keybind or invoked
through `umbriel msg`. See [Keybinds](keybinds.md) for binding syntax.

## Parameterized actions

| Action | Parameter | Example |
|--------|-----------|---------|
| `spawn:<cmd>` | Shell command | `"spawn:kitty"` |
| `submap:<name>` | Enter a named submap; `submap:reset` exits one level | `"submap:resize"` |
| `workspace-switch:<ws>` | Workspace name, optionally `/<output>` | `"workspace-switch:3"`, `"workspace-switch:CHAT/HDMI-A-1"` |
| `window-move-to-workspace:<ws>` | Same as above | `"window-move-to-workspace:2"` |
| `column-move-to-workspace:<ws>` | Same as above; moves the focused window's whole column, except in master layout where it moves only the focused window | `"column-move-to-workspace:CHAT/HDMI-A-1"` |
| `window-set-width:<frac>` | Fraction 0.1-1.0 | `"window-set-width:0.667"` |
| `window-modify-width:<delta>` | Signed fraction -0.9..0.9; the resulting width clamps to 0.1..1.0 | `"window-modify-width:-0.2"` |
| `workspace-set-layout:<scrolling\|dwindle\|master\|toggle>` | Switch the active workspace's layout at runtime; `toggle` cycles scrolling to dwindle to master to scrolling. The override remains until a config reload reasserts the configured mode. | `"workspace-set-layout:toggle"` |
| `window-focus:<window-id>` | Window id from `umbriel windows` | `"window-focus:0123abcd"` |
| `window-focus-warp:<window-id>` | Focus the window and warp the cursor to its visible center | `"window-focus-warp:0123abcd"` |
| `window-close[:<window-id>]` | Optional window id; bare form closes the focused window | `"window-close"` |
| `dpms-off[:<output>]` / `dpms-on[:<output>]` | Optional connector name; bare form targets every configured output | `"dpms-off:DP-1"`, `"dpms-on"` |
| `session-quit[:skip-confirmation]` | Bare form opens an on-screen confirmation (Enter or the quit bind confirms; any other key or click cancels); `skip-confirmation` quits immediately | `"session-quit:skip-confirmation"` |

A second `session-quit` while the confirmation is open also quits. While the
session is locked, `session-quit` quits without the dialog.

Workspace selectors first resolve exact names globally, including numeric
names. A unique name selects its workspace on any output. Duplicate names
resolve on the preferred output. When no exact numeric name exists, the number
selects that 1-based position on the preferred output. On a dynamic output, a
number beyond the current workspace list selects the last workspace. Add
`/output` to target another output explicitly.

When `workspace-switch`, `window-move-to-workspace`, or
`column-move-to-workspace` targets another monitor, the cursor warps to that
monitor's center so subsequent actions continue there.

## Window and layout actions

Unless shown with a `:<parameter>` suffix below, these take no argument.

### Focus

- **Within a row:** `window-focus-left`, `window-focus-right`. Move focus to the
  adjacent window.
- **At a row's output edge:** `window-focus-or-output-left`,
  `window-focus-or-output-right`. Move focus to the adjacent window, or to the
  output in that direction when already at the edge.
- **First or last column:** `column-focus-first`, `column-focus-last`. Move
  focus to the first or last column in the workspace.
- **Within a column:** `window-focus-up`, `window-focus-down`. Move focus to
  the adjacent window.
- **At a workspace boundary:** `window-focus-or-workspace-up`,
  `window-focus-or-workspace-down`. Move within the column, or switch to the
  adjacent workspace and restore its focus.
- **At a column's output edge:** `window-focus-or-output-up`,
  `window-focus-or-output-down`. Move focus to the adjacent window, or to the
  output in that direction when already at the edge.
- **Next or previous window:** `window-focus-next`, `window-focus-previous`.
  Cycle through tiled windows in layout order, then floating windows, with
  wrapping in both directions.

With `input.cursor.follows_focus` enabled, these navigation actions warp the
cursor to the visible center of the selected window. This also applies to
`window-focus-switch-floating`. Pointer-driven and automatic focus changes do
not move the cursor. `window-focus:<id>` remains focus-only, while
`window-focus-warp:<id>` always moves it.

### Moving windows and columns

- **To a selected workspace:** `window-move-to-workspace:<ws>` moves the focused
  window, while `column-move-to-workspace:<ws>` moves its whole column. Both
  use the workspace selectors described above and follow the moved focus.
- **To the next or previous workspace:** `window-move-to-workspace-next` and
  `window-move-to-workspace-previous` move the focused window.
  `column-move-to-workspace-next` and `column-move-to-workspace-previous` move
  its whole column. All four follow the moved focus and do not wrap around.
  In master layout, the column-scoped forms move only the focused window.
- **A column within a row:** `column-move-left`, `column-move-right`. Move the
  focused window's column left or right.
- **A column across an output edge:** `window-move-or-output-left`,
  `window-move-or-output-right`. Move the focused column left or right, or to
  the output in that direction when already at the edge.
- **First or last column position:** `column-move-to-first`,
  `column-move-to-last`. Move the focused window's column to the first or last
  position in the workspace.
- **Next or previous layout position:** `window-swap-next`,
  `window-swap-previous`. Exchange the focused tiled window with its next or
  previous layout-order neighbor, wrapping at both ends. Focus stays on the
  moved window.
- **Master count:** `master-count-increase` promotes the stack's top window to
  the bottom of master. `master-count-decrease` demotes the bottom master window
  to the top of the stack. The minimum master count is one.
- **Within a column:** `window-move-up`, `window-move-down`. Move the focused
  window up or down within its column.
- **Across a workspace boundary:** `window-move-or-workspace-up`,
  `window-move-or-workspace-down`. Move within the column, or move the focused
  window to the adjacent workspace at the boundary.
- **Across an output edge:** `window-move-or-output-up`,
  `window-move-or-output-down`. Move within the column, or move the column to
  the output in that direction when already at the edge.
- **Merge or split columns:** `window-consume-left` pulls the focused window
  into the column to its left. `window-expel-right` places it in a new column to
  the right.

### Size, state, and viewport

- **Column width:** `window-modify-width:<delta>` changes the focused area's
  width by a signed fraction. `window-cycle-width` and
  `window-cycle-width-back` cycle through preset widths in either direction.
- **Fullscreen:** `window-toggle-fullscreen`. Toggle fullscreen for the focused
  window.
- **Column width state:** `window-toggle-maximize`. Toggle the focused column's
  full-width state.
- **Window to usable-area edges:** `window-toggle-maximize-to-edges`. Toggle
  maximization without gaps or borders. Layer-shell exclusive zones remain
  visible. A column's full-width restore state is preserved when this is toggled
  or when fullscreen is entered and left.
- **Center a column:** `column-center`. Center the focused column in the
  scrolling viewport. It is a no-op on non-scrolling workspaces.
- **Scroll the viewport:** `layout-scroll-left`, `layout-scroll-right`. Scroll
  the active workspace's scrolling-layout viewport. `layout-scroll-up` and
  `layout-scroll-down` are first-class synonyms for left and right.

### Configuration

- **Reload configuration:** `config-reload`. Reload the config file, the same
  reload that runs automatically when the file changes on disk.

On a vertical scrolling workspace, directional actions follow their visual
directions. `window-focus-left` and `window-focus-right` move within a lane;
`window-focus-up` and `window-focus-down` walk lanes. Likewise,
`column-move-left` and `column-move-right` reorder within a lane, while
`window-move-up` and `window-move-down` move the lane along the strip.
`layout-scroll-left` and `layout-scroll-up` both scroll toward strip start;
their right and down forms scroll toward strip end.

The default Mod+wheel bindings invoke `window-focus-left` and
`window-focus-right`, so they move within a lane on a vertical workspace.
Vertical-heavy configurations should bind wheel chords to
`window-focus-up` and `window-focus-down`, or to `layout-scroll-up` and
`layout-scroll-down`.

## Floating action

`window-toggle-floating` remembers the window's floating size and position.
The first time a window floats, Umbriel places it slightly below and to the
right of its tiled position while keeping it on-screen.

`window-focus-switch-floating` switches focus to the most recently focused
window with the opposite floating state.

`window-toggle-pinned` makes the focused window float and keeps it above
fullscreen windows on its output. Pinned windows remain visible when you
switch workspaces. You cannot pin a fullscreen window, and making a pinned
window fullscreen removes its pinned state.

## Output and movement actions

`workspace-next` and `workspace-previous` switch to the adjacent workspace on the
focused output, by index. They do not wrap around: `workspace-previous` on the
first workspace is a silent no-op. On a dynamic output, `workspace-next` reaches
the trailing empty workspace, which becomes active as usual.

`workspace-move-down` and `workspace-move-up` move the focused workspace up or down
on the focused output. They do not wrap around either.

The matching window and column actions can be bound independently:

```toml
[keybinds]
"Mod+Shift+Comma" = "window-move-to-workspace-previous"
"Mod+Shift+Period" = "window-move-to-workspace-next"
"Mod+Ctrl+Page_Up" = "column-move-to-workspace-previous"
"Mod+Ctrl+Page_Down" = "column-move-to-workspace-next"
```

Whole-column moves between scrolling workspaces preserve member order and
scrolling-layout state, including the column width, its full-width restore
value, and stacked row proportions. Destination-moving column actions act like
their matching window action when a floating window is focused because it has
no tiled column.
In master layout, column-scoped workspace moves transfer only the focused
window because the master and stack areas are not movable columns.

`window-center` centers the focused floating window on its output's usable
area. It is a no-op while a tiled window is focused.

The directional output actions target the adjacent monitor:

| Action | What it does |
|--------|--------------|
| `output-focus-left` / `output-focus-right` / `output-focus-up` / `output-focus-down` | Move focus to the adjacent monitor in that direction. |
| `window-move-to-output-left` / `window-move-to-output-right` / `window-move-to-output-up` / `window-move-to-output-down` | Move the focused window to the adjacent monitor's active workspace. |
| `column-move-to-output-left` / `column-move-to-output-right` / `column-move-to-output-up` / `column-move-to-output-down` | Move the focused window's whole column to the adjacent monitor's active workspace. In master layout, move only the focused window. |
| `workspace-move-to-output-left` / `workspace-move-to-output-right` / `workspace-move-to-output-up` / `workspace-move-to-output-down` | Move every window of the active workspace to the adjacent monitor, preserving column order and widths. |

Directions do not wrap around: with no monitor in that direction the action
fails with an IPC error ("no output to the left" and friends). The cursor warps
to the center of the target monitor, so focus follows the action. Floating
windows keep their relative position on the new monitor; a column moved onto a
dwindle workspace flattens into single-window columns, whether it is moved by a
workspace or output action, the same as drag-and-drop.

## Overview actions

Use `overview-toggle`, `overview-open`, or `overview-close`.

Windows can be dragged onto another workspace preview. With dynamic numbered
workspaces, dropping a window into the gap between two previews, or into the
gap above the first preview, creates a new workspace at that position and
shifts the following workspace numbers down.
Umbriel keeps one empty dynamic workspace, so other previews disappear as soon
as their last window is moved or closed, including while the overview is open.
Static configured workspace lists only accept drops onto existing previews.

## Cheatsheet actions

Use `cheatsheet-toggle`, `cheatsheet-open`, or `cheatsheet-close`.

The cheatsheet lists every active keybind. It opens at startup when
`general.show_cheatsheet` is `true`, which is the default. You can also toggle
it through IPC with `umbriel msg cheatsheet-toggle`.

Any non-modifier key or mouse button closes the cheatsheet. Bound key
combinations still run normally. A click used to close the cheatsheet is not
passed to the window beneath it.

## Keyboard layout action

`keyboard-layout-next` activates the next layout in `input.keyboard.layout` and
wraps at the end, on every physical keyboard. It is inert when only one layout
is configured, and virtual keyboards keep the keymap their client supplied.

```toml
[input.keyboard]
layout = "us,de"

[keybinds]
"Mod+Shift+K" = "keyboard-layout-next"
```

`umbriel msg keyboard-layout-next` does the same from a script or panel. An XKB
toggle such as `options = "grp:alt_shift_toggle"` is an alternative that lives
in the keymap itself; the two can coexist.

## Scratchpad actions

Each output has a holding area for windows that should stay nearby without
remaining on a workspace.

| Action | What it does |
|--------|--------------|
| `window-move-to-scratchpad` | Move the focused window from its workspace into the scratchpad. |
| `scratchpad-toggle` | Show or hide the output's scratchpad windows. |
| `window-restore-from-scratchpad` | Return the focused scratchpad window to its saved workspace. |
| `window-toggle-scratchpad` | Move the focused window into the scratchpad, or restore it if it's already the scratchpad's focused window. |
| `scratchpad-focus-next` | Focus the next visible scratchpad window. |

Add `:<output>` to any action to target a specific output, for example
`scratchpad-toggle:DP-1`. Without a suffix, the action targets the output under
the pointer.

See [Scratchpads](scratchpad.md) for setup examples, the full workflow,
multi-output behavior, restoration rules, and troubleshooting.
