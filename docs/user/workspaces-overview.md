# Workspaces Overview

The overview displays every workspace and lets you navigate, focus, close, and move windows.

## Settings and behavior

```toml
[overview]
zoom = 0.5
zoom_presets = [0.5, 0.25, 0.1]
zoom_scroll_factor = 1.0
scroll_factor_horizontal = 1.0
scroll_factor_vertical = 1.0
background_blur = true
workspace_wallpaper = true
shortcuts = true
shortcut_keys = "1234567890"
```

| Key                        | Default            | Description                                                                                |
| -------------------------- | ------------------ | ------------------------------------------------------------------------------------------ |
| `zoom`                     | `0.5`              | Workspace preview scale from 0.1 to 0.75.                                                  |
| `zoom_presets`             | `[0.5, 0.25, 0.1]` | Zoom levels in cycling order, each from 0.1 to 0.75. An empty list disables zoom actions.  |
| `zoom_scroll_factor`       | `1.0`              | Sensitivity of smooth zoom scroll bindings, from 0.1 to 10. Lower values zoom more slowly. |
| `scroll_factor_horizontal` | `1.0`              | Horizontal gesture and wheel sensitivity.                                                  |
| `scroll_factor_vertical`   | `1.0`              | Vertical gesture and wheel sensitivity.                                                    |
| `background_blur`          | `true`             | Blur the desktop behind the overview.                                                      |
| `workspace_wallpaper`      | `true`             | Show each output's background inside its workspace previews.                               |
| `shortcuts`                | `true`             | Show and accept keyboard shortcut badges.                                                  |
| `shortcut_keys`            | `"1234567890"`     | Preferred keys for shortcut badges.                                                        |

Background blur uses `[appearance.blur]`. Preview backgrounds use `colors.overview.workspace_background` when wallpaper mirroring is disabled or no background surface is available.

### Open and navigate

Press `Mod+O` by default, or use an [overview action](actions.md#overview). Opening the overview temporarily hides scratchpads and pinned windows.

- Click a window to focus it and close the overview.
- Middle-click a window to close it.
- Drag a window to move it to another workspace.
- Use the wheel to navigate vertically and Shift+wheel horizontally.
- Use a four-finger swipe to open or close the overview.

Two-finger scrolling and three-finger swipes navigate continuously. Movement along the output's [workspace axis](workspaces.md#workspace-axis) moves between workspaces. Movement across it pans a scrolling workspace preview. Dwindle and Master layouts have no strip to pan.

While a window card drag is held, scrolling stays live and both dimensions use the same mechanic: two-finger travel pans the layout strip across the workspace axis and the workspace rows along it, and holding the dragged card near a screen edge slides whichever dimension runs toward it with [drag edge scroll](input.md#drag-edge-scroll) (the strip at its ends, the workspace rows at theirs). The rows never step to another workspace under the hold; releasing drops the window on the workspace under the pointer, which then becomes active.

Touchpad `natural_scroll` controls gesture direction. The horizontal and vertical overview factors adjust gesture distance and wheel sensitivity without changing application scrolling.

#### Which window actions act on

One card carries the full focused-border color while the overview is open. It is the target for focus, close, and other window actions. Each other workspace keeps a fainter marker showing which card will receive focus when selected.

The current output follows the cursor. Output-changing actions move the cursor to their destination as usual.

#### Keyboard shortcuts

Normal `[keybinds]` remain active in the overview. Directional focus actions select neighboring cards, and workspace or output actions keep their normal fallback behavior.

Shortcut badges provide direct, unmodified key sequences for visible window cards. When there are more cards than keys, Umbriel creates multi-key labels. Backspace removes the last key from a pending sequence. Escape clears the sequence first and closes the overview when no sequence is pending.

Set `shortcuts = false` to disable badges. A normal keybind takes precedence over a badge. `shortcut_keys` must contain at least two unique printable ASCII characters.

### Zoom in and out

Keyboard bindings and IPC calls to `overview-zoom-in` and `overview-zoom-out` step through `zoom_presets` by size. Zoom-out opens a closed overview at the largest preset; zoom-in past the largest preset closes it. Zoom-out stops at the smallest preset. `overview-zoom-cycle` follows the configured list order and wraps, opening at the first preset when closed. Duplicate levels are ignored.

For example, these bindings replace the default Mod+wheel focus actions:

```toml
[keybinds]
"Mod+WheelUp" = "overview-zoom-in"
"Mod+WheelDown" = "overview-zoom-out"
"Mod+Shift+O" = "overview-zoom-cycle"
```

When bound to a scroll direction, zoom-in/out follow the scroll amount continuously between the smallest and largest presets. Fractional wheel movement and two-finger scrolling with the bound modifier both work without waiting for a full notch. At the default `zoom_scroll_factor = 1.0`, one notch multiplies the scale by 1.1 when zooming in or divides it by 1.1 when zooming out. Opposite travel reverses the change. Scrolling out from the desktop opens at the largest preset; scrolling in beyond that preset closes the overview.

For finer control, set `zoom_scroll_factor = 0.5`; use `2.0` for faster zoom. Keep `cooldown_ms` unset on smooth zoom bindings so every scroll sample can take effect. Keyboard shortcuts still use the presets, and `overview-zoom-cycle` still cycles presets even when bound to a wheel.

Zoom changes apply to every output and use `[animation.overview]` timing. They preserve the selected workspace and window. Ordinary wheel and touchpad navigation continues at the new scale. Zoom actions are ignored during a window drag or middle-button navigation.

Opening with `overview-open`, `overview-toggle`, or a four-finger swipe starts at `overview.zoom`. Closing uses the currently displayed scale, including when a zoom animation is interrupted.

### Move windows

Drag a window onto another workspace preview to move it. In a dynamic workspace list, dropping into a gap creates a workspace at that position. Static workspace inventories accept drops only onto existing previews.

The destination layout shows an insertion preview before the drop. Empty dynamic workspaces may disappear immediately after their last window is moved or closed.

### Appearance

Overview cards reuse each window's borders, corner radius, opacity, blur, and color presentation. Shortcut badges use `colors.overview.badge`.

Configure overview colors under [`[colors.overview]`](appearance.md#overview-colors).
