# Workspaces Overview

The overview displays every workspace and lets you navigate, focus, close, and move windows.

## All-window spread

`umbriel msg overview-spread` toggles a fullscreen spread of windows from every workspace and output on the invoking output. It shares the native overview's live client buffers, opacity, backdrop blur, decorations, and effects. Videos continue drawing at the output's frame cadence, without a screenshot refresh limit.

Windows travel from their desktop positions. Inactive workspaces supply virtual origins above or below, or to the left or right, following each output's `workspace_axis`. The grid assigns windows to nearby slots to reduce crossings. Every output keeps its wallpaper and dims during the transition.

Click a window or press Enter to activate it and return to its workspace. Arrows and Tab select cards; the X button, middle-click, or Delete closes a window. Escape, clicking the background, or invoking `overview-spread` again dismisses the spread. The normal overview shortcut badges also work. Attached dialogs appear with their parent, and scratchpads retain the workspace overview's hide-on-open behavior. Pinned and minimized windows are included.

The workspace overview remains available through `overview-open` and `overview-toggle`. The spread keeps the existing overview animation curve and enable switches, with independent entry and exit timing in `overview.spread.duration_ms`. Its window grid does not use the workspace `zoom` or scroll settings.

With Noctalia's native spread integration, bind `Super+W` to `spawn noctalia msg window-switcher overview`; Alt+Tab can continue using Noctalia's carousel independently. Shells can distinguish the modes through the overview event's `mode` field and leave the keyboard with Umbriel during the spread.

### Spread settings

These settings are independent of the workspace filmstrip. They can also be changed at runtime with `umbriel config set overview.spread.<key> <value>`.

```toml
[overview.spread]
duration_ms = 250
all_workspaces = true
all_outputs = true
spacing = 32.0
max_scale = 1.0
dim = 0.4
wallpaper_dim = 0.0
show_titles = true
show_workspace_numbers = true
show_close_buttons = true
background_blur = true
blur_strength = 1.0
inset_wallpaper = false
```

`all_outputs = false` spreads each monitor’s windows on that monitor. `all_outputs = true` gathers windows from every monitor onto the invoking monitor. `all_workspaces = false` includes only the active workspace on each included monitor, including its pinned windows. Other windows fade into the wallpaper during entry and return during dismissal.

`duration_ms` sets the entry and exit duration from 0 to 2000 milliseconds (default 250). Zero makes these transitions instant. Windows and the background share this timeline. The configured overview curve keeps its shape, including spring curves, fitted to the selected duration. Workspace overview timing remains independent.

`spacing` controls the grid gap (0–128 logical pixels, adjusted for density). `max_scale` limits window previews to 10–100% of their original size. Smaller windows are never enlarged. Titles, workspace numbers, and close buttons can be hidden independently. Keyboard closing remains available.

`dim` controls full-screen backdrop darkness from 0 to 1 across all monitors. `wallpaper_dim` independently darkens the inset wallpaper from 0 to 1, leaving window contents unchanged. `background_blur` enables background blur using `[appearance.blur]`, which must also be enabled; `blur_strength` scales it from 0 to 1. This does not change client-window transparency or blur.

`inset_wallpaper = true` (Overview Style Wallpaper in Noctalia) animates a rounded, slightly smaller wallpaper above the dimmed, optionally blurred full-screen backdrop. Every monitor gets this effect in both monitor modes. When windows are gathered onto the invoking monitor, the other monitors show their own rounded wallpaper with the blurred or dimmed backdrop around it. The inset centers within the usable work area, respecting panels on any edge. It starts at 70% of that area and expands as needed to surround the existing windows and captions with 16-pixel padding. A soft shadow separates it from the blurred outer wallpaper. Window sizes, positions, and gaps stay independent of this frame; the wallpaper is cropped to fill without stretching. Turn it off for the full-screen wallpaper style.

## Settings and behavior

```toml
[overview]
zoom = 0.5
scroll_factor_horizontal = 1.0
scroll_factor_vertical = 1.0
background_blur = true
workspace_wallpaper = true
shortcuts = true
shortcut_keys = "1234567890"
```

| Key                        | Default        | Description                                                  |
| -------------------------- | -------------- | ------------------------------------------------------------ |
| `zoom`                     | `0.5`          | Workspace preview scale from 0.1 to 0.75.                    |
| `scroll_factor_horizontal` | `1.0`          | Horizontal gesture and wheel sensitivity.                    |
| `scroll_factor_vertical`   | `1.0`          | Vertical gesture and wheel sensitivity.                      |
| `background_blur`          | `true`         | Blur the desktop behind the overview.                        |
| `workspace_wallpaper`      | `true`         | Show each output's background inside its workspace previews. |
| `shortcuts`                | `true`         | Show and accept keyboard shortcut badges.                    |
| `shortcut_keys`            | `"1234567890"` | Preferred keys for shortcut badges.                          |

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

### Move windows

Drag a window onto another workspace preview to move it. In a dynamic workspace list, dropping into a gap creates a workspace at that position. Static workspace inventories accept drops only onto existing previews.

The destination layout shows an insertion preview before the drop. Empty dynamic workspaces may disappear immediately after their last window is moved or closed.

### Appearance

Overview cards reuse each window's borders, corner radius, opacity, blur, and color presentation. Shortcut badges use `colors.overview.badge`.

Configure overview colors under [`[colors.overview]`](appearance.md#overview-colors).
