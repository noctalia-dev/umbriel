# Placement rules

Placement keys decide where and how a window opens. They apply once, when it opens.

```toml
# Float the volume mixer at half the screen
[[window_rule]]
match.app_id = "^org[.]pulseaudio[.]pavucontrol$"
default_floating = true
default_floating_size = { width = 0.5, height = 0.6 }
```

## Placement keys

| Key                              | Value                              | Description                                                       |
| -------------------------------- | ---------------------------------- | ----------------------------------------------------------------- |
| `default_output`                 | connector or monitor name          | Open on this output.                                              |
| `default_workspace`              | position or name                   | Open on this workspace.                                           |
| `default_scratchpad`             | scratchpad name                    | Store in a scratchpad.                                            |
| `default_floating`               | bool                               | Force floating or tiling.                                         |
| `default_floating_size`          | `{ width, height }` fractions      | Floating size as a fraction of the usable area.                   |
| `default_floating_size_px`       | `{ width, height }` logical pixels | Floating size in pixels.                                          |
| `default_position`               | `{ x, y, anchor }`                 | Floating position from a named anchor.                            |
| `default_scrolling_extent`       | 0.1 to 4.0                         | Initial scrolling column width as a fraction of the viewport.     |
| `default_scrolling_extent_px`    | logical pixels, up to 4 viewports  | Initial scrolling column width in pixels.                         |
| `default_scrolling_column`       | name                               | Join matching windows into one named scrolling column.            |
| `default_scrolling_column_order` | integer                            | Order inside a named scrolling column.                            |
| `default_column_display`         | `"normal"` or `"tabbed"`           | Show the column the window opens in stacked or as tabs.           |
| `default_fullscreen`             | bool                               | Open fullscreen.                                                  |
| `default_maximize`               | bool                               | Open maximized within the layout's gaps.                          |
| `default_maximize_to_edges`      | bool                               | Open maximized without gaps, struts, or borders.                  |
| `default_focused`                | bool                               | Override whether the new window takes focus.                      |
| `default_pinned`                 | bool                               | Open pinned above normal windows.                                 |
| `attach_to_parent`               | bool, default `true`               | Let a modal dialog attach to its parent. See [Dialogs](#dialogs). |

> [!NOTE]
> Some applications, such as Wine and Proton games, publish their title or content type only after they open. Opening rules are checked again once when that arrives; each value still applies at most once.

## Open on a workspace or output

An integer is a one-based workspace position. A string is an exact, case-sensitive workspace name, which must already exist. `default_output` restricts either form to one output.

```toml
default_workspace = 2
# default_workspace = "2"
# default_workspace = "CHAT"
```

```toml
# Steam games open fullscreen on workspace 4
[[window_rule]]
match.app_id = "^steam_app_[0-9]+$"
default_workspace = 4
default_fullscreen = true
```

## Float, size, and position

- `_px` sizes take precedence over fractions, and each axis is optional.
- A tiled window remembers its floating size and position until it first floats. A floating window remembers its scrolling width until it first tiles.
- The application can still resize itself afterward.

```toml
# A utility at a fixed size
[[window_rule]]
match.app_id = "^org[.]example[.]Utility$"
default_floating = true
default_floating_size_px = { width = 800, height = 600 }
```

`default_position` places a floating window in logical pixels within the usable area:

```toml
default_position = { x = 32, y = 24, anchor = "bottom_left" }
```

Anchors are `center`, `top_left`, `top_right`, `bottom_left`, `bottom_right`, `top`, `bottom`, `left`, and `right`. Right and bottom anchors measure inward from those edges.

## Scrolling columns

`default_scrolling_extent` and `default_scrolling_extent_px` set the width a new column opens with. Give related applications the same column name to stack them in one column:

```toml
# Browsers share one column, Firefox on top
[[window_rule]]
match.app_id = "^firefox$"
default_scrolling_column = "browsers"
default_scrolling_column_order = 10

[[window_rule]]
match.app_id = "^chromium$"
default_scrolling_column = "browsers"
default_scrolling_column_order = 20
```

The name is local to a workspace. The first matching window creates the column and sets its width. Add `default_column_display = "tabbed"` to keep the column as [tabs](tab-groups.md).

## Maximize, fullscreen, and focus

- `default_maximize` opens the window at full column width.
- `default_maximize_to_edges` also drops gaps, struts, and borders.
- The application's own maximize request follows [`client_maximize`](window-states.md#maximize-and-fullscreen) instead.
- `default_focused = true` focuses and reveals a floating window that opens above a fullscreen one.
- `default_pinned = true` opens the window floating and pinned.

## Open in a scratchpad

```toml
# A terminal that lives in its own scratchpad
[[scratchpad]]
name = "terminal"

[[window_rule]]
match.app_id = "^scratchpad-terminal$"
default_scratchpad = "terminal"
default_floating_size = { width = 0.7, height = 0.7 }
```

- Without named scratchpads, use `"default"`.
- A hidden scratchpad keeps the new window hidden unless `default_focused = true` is also set.
- The window opens with its `default_floating_size*` and `default_position`, centered without a position, or fullscreen or maximized to the edges with those keys. A window moved into a scratchpad later keeps its size.
- Output, workspace, and floating rules decide where and how it returns when restored.

## Rules for a lone window

`match.is_alone` holds while a window is alone in its workspace's tiled layout:

```toml
# Maximize a window while it is alone
[[window_rule]]
match.is_alone = true
default_maximize = true
```

- In scrolling, every window in the only column matches together. `default_maximize` and `default_scrolling_extent` act on that column.
- In master and dwindle, it means the only tiled window.
- Fullscreen and maximize-to-edges also need the window to be the only tile.
- The effect is removed when another column appears, and restored when the window is alone again.
- A `default_pinned` window opens floating, so it never matches.

```toml
# A wider Firefox while it is alone
[[window_rule]]
match.is_alone = true
match.app_id = "^firefox$"
default_scrolling_extent = 0.8
```

Floating windows do not count here. To ask whether no other window is on the workspace at all, use `match.is_only_window`; see [Borders that show window state](window-rules-appearance.md#borders-that-show-window-state).

## Dialogs

- Dialogs float by default. `default_floating = false` puts one into the layout.
- A modal dialog [attaches to its parent](window-states.md#dialogs) and ignores placement rules. `attach_to_parent = false` keeps it an ordinary window; this is re-checked on reload.
- A rule matching the parent also covers its dialogs. A rule matching the dialog itself wins.

File pickers from the desktop portal carry the portal's app ID, such as `xdg-desktop-portal-gtk`, so match the parent application to change them:

```toml
# Keep Firefox's dialogs as ordinary windows
[[window_rule]]
match.app_id = "^org\\.mozilla\\.firefox$"
attach_to_parent = false
```

> [!NOTE]
> X11 menus, tooltips, dropdowns, and notifications are not managed windows. Rules do not apply to them, and they do not appear in `umbriel windows`.
