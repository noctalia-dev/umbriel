# Window rules

A window rule applies settings to the windows it matches.

```toml
# Open Firefox on workspace 2
[[window_rule]]
match.app_id = "^firefox$"
default_workspace = 2
```

The keys are grouped by what they change: [Placement rules](window-rules-placement.md), [Appearance rules](window-rules-appearance.md), and [Behavior rules](window-rules-behavior.md).

## How rules combine

- Every matching rule contributes its keys. When two rules set the same key, the later rule wins.
- A rule with no `match.*` selector matches every window.
- Placement keys (`default_*`) apply once, when a window opens. All other keys follow the window: they update when its identity or state changes, and on config reload.
- A rule cannot select on a floating or pinned state that it creates itself.

## Common tasks

| Task                                  | Keys                                        | Page                                                                             |
| ------------------------------------- | ------------------------------------------- | -------------------------------------------------------------------------------- |
| Open an app on a workspace or monitor | `default_workspace`, `default_output`       | [Placement](window-rules-placement.md#open-on-a-workspace-or-output)             |
| Float a utility at a set size         | `default_floating`, `default_floating_size` | [Placement](window-rules-placement.md#float-size-and-position)                   |
| Send an app to a scratchpad           | `default_scratchpad`                        | [Placement](window-rules-placement.md#open-in-a-scratchpad)                      |
| Keep two apps in one scrolling column | `default_scrolling_column`                  | [Placement](window-rules-placement.md#scrolling-columns)                         |
| Fade unfocused windows                | `opacity`, `match.is_focused`               | [Appearance](window-rules-appearance.md#fade-unfocused-windows)                  |
| Remove borders and shadow from an app | `border_width`, `corner_radius`, `shadow`   | [Appearance](window-rules-appearance.md#applications-with-their-own-decorations) |
| Show title bars per app or layout     | `title_bar`                                 | [Appearance](window-rules-appearance.md#title-bars)                              |
| Blur a translucent app                | `blur`, `blur_ignore_alpha`                 | [Appearance](window-rules-appearance.md#transparent-content-and-blur)            |
| Control supersampling per app         | `supersample`                               | [Behavior](window-rules-behavior.md#supersampling)                               |
| Tune a game: VRR, tearing, pointer    | `vrr`, `tearing`, `confine_pointer`         | [Behavior](window-rules-behavior.md#games)                                       |
| Keep shortcuts in a virtual machine   | `allow_shortcuts_inhibit`                   | [Behavior](window-rules-behavior.md#keep-compositor-shortcuts)                   |
| Hide a window from screen sharing     | `block_capture`                             | [Behavior](window-rules-behavior.md#hide-a-window-from-screenshots)              |

## Match windows

Patterns are regular expressions that match any part of the value; use `^` and `$` for an exact match. `match.app_id`, `match.title`, `match.xdg_tag`, `match.output`, and `match.layout` also take an array: any entry matching is enough, and an empty array places no constraint.

```toml
# Scanlines on two video players
[[window_rule]]
match.app_id = ["^mpv$", "^celluloid$"]
window_effect = "scanlines"
```

> [!TIP]
> Run `umbriel windows --json | jq` to see every field of every window, labeled. To inspect one window, run `sleep 3; umbriel focused-window --json | jq` and focus that window before the delay ends. Prefer `match.app_id`: it stays the same for the life of a window, while many apps change their title as you use them.

### Identity selectors

| Selector             | Matches                                               |
| -------------------- | ----------------------------------------------------- |
| `match.app_id`       | The application ID.                                   |
| `match.title`        | The window title.                                     |
| `match.xdg_tag`      | A tag the application sets on its window.             |
| `match.content_type` | `"none"`, `"photo"`, `"video"`, or `"game"`.          |
| `match.is_dialog`    | Whether the application named a parent window for it. |
| `match.at_startup`   | Whether the session is within its first 60 seconds.   |

`match.title = "^$"` matches an empty title. The `-` shown for untitled windows in `umbriel windows` is only a placeholder and does not match.

### State selectors

These update while the window is open.

| Selector                      | Matches                                                                                                                                                    |
| ----------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `match.output`                | The output the window is on, or for an opening window, the output its placement rules select. See [Per-monitor rules](#per-monitor-rules).                 |
| `match.layout`                | The workspace layout: `"scrolling"`, `"dwindle"`, `"master"`, or `"stacking"`.                                                                             |
| `match.is_focused`            | Focus.                                                                                                                                                     |
| `match.is_floating`           | Floating.                                                                                                                                                  |
| `match.is_maximized`          | Maximized, including maximized to the edges.                                                                                                               |
| `match.is_maximized_to_edges` | Maximized to the edges.                                                                                                                                    |
| `match.is_pinned`             | Pinned.                                                                                                                                                    |
| `match.is_scratchpad`         | In a scratchpad.                                                                                                                                           |
| `match.is_alone`              | Alone in its workspace's tiled layout. See [Rules for a lone window](window-rules-placement.md#rules-for-a-lone-window).                                   |
| `match.is_only_window`        | The only window on its workspace, floating ones included. See [Borders that show window state](window-rules-appearance.md#borders-that-show-window-state). |
| `match.is_cast_target`        | Captured by a window share or window screenshot. See [Borders that show window state](window-rules-appearance.md#borders-that-show-window-state).          |

> [!NOTE]
> `match.layout` follows the workspace layout, floating windows included. A window without a workspace does not match it, so use it for live settings, not for where a window opens.

### Per-monitor rules

`match.output` searches both the output's connector name (`HDMI-A-1`) and its monitor descriptor (`<make> <model> <serial>`, as `umbriel outputs` prints it), so either form matches. A display that reports no EDID fields only answers to its connector.

```toml
[[window_rule]]
match.app_id = "^obsidian$"
match.output = "AU Optronics"
default_scrolling_extent = 1
```

Placement keys resolve against the output the window opens on, so a rule that also moves the window (`default_output`, `default_workspace`, `default_scratchpad`) is matched against where the window lands, not where it started. All other keys follow the window's current output, so moving a window to another monitor re-resolves them.
