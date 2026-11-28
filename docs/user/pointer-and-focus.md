# Pointer and focus

Configure how focus follows the mouse, how the cursor behaves, and how windows are moved and resized with the mouse.

```toml
# Focus the window under the pointer
[input.focus]
follows_mouse = "sloppy"
```

## Focus follows mouse

```toml
[input.focus]
follows_mouse = "off"
follows_mouse_max_scroll = 0.5
hover_raises = true
```

| Value      | Behavior                                                                                                             |
| ---------- | -------------------------------------------------------------------------------------------------------------------- |
| `"off"`    | Only clicks, keybinds, and activation move focus.                                                                    |
| `"sloppy"` | Moving onto a window focuses it. Focus given elsewhere, by a keybind or a new window, stays until the pointer moves. |
| `"always"` | Every pointer motion focuses the window under the pointer, including inside the window it is already over.           |

| Key                        | Default | Description                                                                                                                                   |
| -------------------------- | ------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `follows_mouse`            | `"off"` | One of the values above.                                                                                                                      |
| `follows_mouse_max_scroll` | unset   | How far, in viewport widths, the layout may scroll to reveal a hovered window. `0.0` allows only fully visible windows. Unset means no limit. |
| `hover_raises`             | `true`  | Raise the hovered window above its neighbors. `false` keeps hover to focus only; clicks and keybinds still raise.                             |

- Scrolling or rearranging the layout under a still pointer never changes focus by itself. With `"always"`, focus moves to the window under a still pointer once the layout settles.
- While a dialog has focus, moving over its parent keeps the dialog focused, so the pointer can reach it.
- Open menus, such as a tray menu, stay open while you hover other windows.

## Cursor

```toml
[input.cursor]
theme = ""
size = 24
hardware_cursor = true
follows_focus = false
hide_when_typing = false
hide_timeout_ms = 0
hide_on_touch = true
```

| Key                | Default | Description                                                       |
| ------------------ | ------- | ----------------------------------------------------------------- |
| `theme`            | `""`    | Xcursor theme; empty uses the environment default.                |
| `size`             | `24`    | Cursor size in logical pixels, 1 to 512.                          |
| `hardware_cursor`  | `true`  | Use a hardware cursor plane when available.                       |
| `follows_focus`    | `false` | Move the cursor to the window focused from the keyboard.          |
| `hide_when_typing` | `false` | Hide the cursor after a non-modifier key press.                   |
| `hide_timeout_ms`  | `0`     | Hide after this many idle milliseconds, up to 3600000; `0` never. |
| `hide_on_touch`    | `true`  | Hide the cursor while using the touchscreen.                      |

- Set `hardware_cursor = false` if the cursor flickers or disappears.
- A hidden cursor releases hover, so the window under it stops showing hover state until the pointer moves. Games that lock the pointer keep it.
- `follows_focus` moves the cursor on keyboard focus changes and keyboard window moves, to the window's center. New windows, activations, and pointer-driven focus do not move it.
- `window-focus-warp <id>` always moves the cursor.

## Move and resize with the mouse

```toml
[input]
client_window_drag = true
window_drag_toggle = "none"
mod_right_double_click = "none"
```

- Mod+drag moves any window. With an Umbriel [title bar](title-bars.md), dragging the bar moves it and a double-click maximizes it.
- Floating windows resize from their edges and corners without a modifier, with or without a title bar.
- Mod+right-drag resizes tiled windows from the nearest edge or corner.
- A floating window dropped mostly off-screen slides back until at least a quarter of it is visible.
- Stacking workspaces snap windows dropped on an edge or corner; see [Drop zones](layout-stacking.md#drop-zones-and-region-placement).

`client_window_drag = false` ignores move requests from applications, such as dragging their own title bar or the empty tab strip of a browser. Resizing from their borders and tearing off tabs still work.

`window_drag_toggle` sets what the other main mouse button does during a window drag:

| Value        | Behavior                                                |
| ------------ | ------------------------------------------------------- |
| `"none"`     | Nothing.                                                |
| `"floating"` | Toggle whether the dropped window is tiled or floating. |
| `"pinned"`   | Toggle whether the dropped window is pinned.            |

The change happens on drop; an impossible change, such as pinning a fullscreen window, is skipped.

`mod_right_double_click` gives a second Mod+right-click on the same window a job:

| Value        | Behavior                                                                                                                            |
| ------------ | ----------------------------------------------------------------------------------------------------------------------------------- |
| `"none"`     | Mod+right-click only resizes.                                                                                                       |
| `"maximize"` | A second click within 300 ms and 8 px toggles maximize, the same way the app's title bar double-click does (see `client_maximize`). |

With `"maximize"`, resizing starts only once the pointer moves more than 8 px. A `Mod+MouseRight` keybind replaces this button entirely.

## Drag edge scroll

```toml
[input.drag_edge_scroll]
enabled = true
trigger_zone = 30
delay_ms = 100
max_speed = 1500.0
```

Holding a drag near either end of a scrolling layout scrolls it, so offscreen columns can be reached. This works for file drags, tiled window moves, and overview window cards, which also scroll the workspace rows.

| Key            | Default  | Description                                                 |
| -------------- | -------- | ----------------------------------------------------------- |
| `enabled`      | `true`   | Turn edge scrolling on or off.                              |
| `trigger_zone` | `30`     | Width of the zone at each end in logical pixels, 1 to 1000. |
| `delay_ms`     | `100`    | Time in the zone before scrolling starts, 0 to 10000.       |
| `max_speed`    | `1500.0` | Peak speed in logical pixels per second, 1.0 to 100000.0.   |

Speed rises toward the edge of the layout. `delay_ms = 0` starts at once.

## Detached tabs and panels

Applications that support `xdg-toplevel-drag-v1`, such as Chromium-based browsers, can tear off a tab and drop it back onto another window to merge or dock it. No configuration is needed.
