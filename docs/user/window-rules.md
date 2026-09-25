# Window Rules

Window rules apply settings to matching applications. Every matching rule
contributes its values; when two set the same field, the later rule wins.

```toml
[[window_rule]]
match.app_id = "^firefox$"
default_workspace = 2
```

## Matching

| Selector | Description |
| --- | --- |
| `match.app_id` | Match the application ID with a regular expression. |
| `match.title` | Match the window title. |
| `match.xdg_tag` | Match a client-defined XDG toplevel tag. |
| `match.content_type` | Match `"none"`, `"photo"`, `"video"`, or `"game"`. |
| `match.is_focused` | Match current focus state. |
| `match.is_floating` | Match current floating state. |
| `match.is_pinned` | Match current pinned state. |
| `match.is_scratchpad` | Match current scratchpad state. |
| `match.is_alone` | Match whether this is the only tiled window. |
| `match.at_startup` | Match during or after the first 60 seconds. |

Selectors are optional. A rule with none matches every window. Regular
expressions match any part of a value, so use `^` and `$` for an exact match.

Run `umbriel windows` to inspect current application IDs, titles, tags, and
content types. Prefer `app_id` for placement because titles often change.

State selectors update while the window is open. Opening settings are resolved
before a rule's own state changes take effect, so a rule cannot select on the
floating or pinned state that it creates.

## Settings applied when a window opens

| Key | Description |
| --- | --- |
| `default_output` | Open on a connector or monitor name. |
| `default_workspace` | Open on a one-based position or exact workspace name. |
| `default_scratchpad` | Store in the implicit or a configured scratchpad. |
| `default_fullscreen` | Open fullscreen. |
| `default_floating` | Force floating or tiling. |
| `default_maximize` | Open maximized within normal layout bounds. |
| `default_maximize_to_edges` | Open maximized without gaps, struts, or borders. |
| `default_focused` | Choose whether the new window takes focus. |
| `default_pinned` | Open pinned above normal windows. |
| `default_scrolling_column` | Join matching windows into one named scrolling column. |
| `default_scrolling_column_order` | Set order inside a named scrolling column. |

These values apply once when a window opens. Umbriel checks once more when the
first title arrives because some applications set it after mapping.

Dialogs float by default. Use `default_floating = false` in a matching rule to
force one into the layout.

## Size and Position Rules

| Key | Applies to | Description |
| --- | --- | --- |
| `default_floating_size_px` | Floating | Logical-pixel size, such as `{ width = 800, height = 600 }`. |
| `default_floating_size` | Floating | Fraction of the output usable area. |
| `default_scrolling_extent_px` | Scrolling | Initial logical-pixel extent along the strip. |
| `default_scrolling_extent` | Scrolling | Initial fractional extent along the strip. |
| `default_position` | Floating | Position from a named anchor. |

Pixel values take precedence over fractions. Each axis is optional. A tiled
window remembers floating size and position rules until it first floats, and a
floating window remembers its scrolling extent until it first tiles.

```toml
[[window_rule]]
match.app_id = "^org[.]example[.]Utility$"
default_floating = true
default_floating_size_px = { width = 800, height = 600 }
```

### Floating position

Coordinates are logical pixels within the output's usable area:

```toml
default_position = { x = 32, y = 24, anchor = "bottom_left" }
```

Accepted anchors are `center`, `top_left`, `top_right`, `bottom_left`,
`bottom_right`, `top`, `bottom`, `left`, and `right`. Right and bottom anchors
measure inward from those edges.

## Scratchpad placement

Store matching windows directly in a scratchpad:

```toml
[[scratchpad]]
name = "terminal"

[[window_rule]]
match.app_id = "^scratchpad-terminal$"
default_scratchpad = "terminal"
```

With no named definitions, use `"default"`. A hidden scratchpad keeps the new
window hidden. Output, workspace, and floating rules determine where and how it
returns when restored.

## Workspace placement

`default_workspace` uses TOML type to distinguish positions and names:

```toml
default_workspace = 2
# default_workspace = "2"
# default_workspace = "CHAT"
```

An integer selects a one-based position. A string selects an exact,
case-sensitive name. `default_output` restricts either form to one output.
Names must already exist through a static inventory or a named workspace rule.

## Named scrolling columns

Give related applications the same column name:

```toml
[[window_rule]]
match.app_id = "^firefox$"
default_scrolling_column = "browsers"
default_scrolling_column_order = 10

[[window_rule]]
match.app_id = "^chromium$"
default_scrolling_column = "browsers"
default_scrolling_column_order = 20
```

The name is local to a workspace. The first matching window creates the column
and sets its extent.

## Settings updated while a window is open

| Key | Description |
| --- | --- |
| `opacity` | Surface opacity from 0.0 to 1.0. |
| `blur` | Enable or disable window blur. |
| `blur_popups` | Apply blur to descendant XDG popups. |
| `blur_ignore_alpha` | Skip blur below an alpha threshold. |
| `blur_optimized` | Override the global optimized-blur choice. |
| `focus_on_activate` | Override activation focus for this window. |
| `vrr` | Override the focused output's VRR policy. |
| `tearing` | Request or veto asynchronous presentation. |
| `hdr` | Override the focused output's HDR policy. |

These values refresh when matching identity or state changes. Fullscreen
bypasses rule opacity unless
[`appearance.opaque_fullscreen`](appearance.md#window-appearance) is `false`.

## The only window in the workspace

`match.is_alone` reacts when a window becomes the only tiled window on its
workspace:

```toml
[[window_rule]]
match.is_alone = true
default_maximize = true
```

It can apply fullscreen, maximize-to-edges, maximize, or a scrolling extent.
The effect is removed when another tiled window appears and restored when the
window becomes alone again. A window that opens with `default_pinned = true`
opens floating, so it never opens in the alone state.

Combine it with other selectors when only one application should receive the
behavior:

```toml
[[window_rule]]
match.is_alone = true
match.app_id = "^firefox$"
default_scrolling_extent = 0.8
```

## Examples

```toml
# Blur every window
[[window_rule]]
blur = true

# Float a utility
[[window_rule]]
match.app_id = "^org[.]pulseaudio[.]pavucontrol$"
default_floating = true
default_floating_size = { width = 0.5, height = 0.6 }

# Place a game on workspace 4
[[window_rule]]
match.app_id = "^steam_app_[0-9]+$"
default_workspace = 4
default_fullscreen = true

# Enable VRR for game content
[[window_rule]]
match.content_type = "game"
vrr = "always"

# Dim unfocused windows
[[window_rule]]
match.is_focused = false
opacity = 0.85

[[window_rule]]
match.is_focused = true
opacity = 1.0
```
