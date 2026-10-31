# Window Rules

Window rules apply settings to matching applications. Every matching rule contributes its values; when two set the same field, the later rule wins.

```toml
[[window_rule]]
match.app_id = "^firefox$"
default_workspace = 2
```

## Matching

| Selector                      | Description                                                                     |
| ----------------------------- | ------------------------------------------------------------------------------- |
| `match.app_id`                | Match the application ID with a regular expression.                             |
| `match.title`                 | Match the window title.                                                         |
| `match.xdg_tag`               | Match a client-defined XDG toplevel tag.                                        |
| `match.content_type`          | Match `"none"`, `"photo"`, `"video"`, or `"game"`.                              |
| `match.is_focused`            | Match current focus state.                                                      |
| `match.is_floating`           | Match current floating state.                                                   |
| `match.is_maximized_to_edges` | Match current maximized-to-edges state.                                         |
| `match.is_pinned`             | Match current pinned state.                                                     |
| `match.is_scratchpad`         | Match current scratchpad state.                                                 |
| `match.is_alone`              | Match whether this is the only tiled window.                                    |
| `match.is_only_window`        | Match whether this is the only window on its workspace, floating ones included. |
| `match.is_dialog`             | Match whether the application named a parent window for it.                     |
| `match.is_cast_target`        | Match while a screencast or screenshot is capturing this window.                |
| `match.at_startup`            | Match during or after the first 60 seconds.                                     |

Selectors are optional. A rule with none matches every window. Regular expressions match any part of a value, so use `^` and `$` for an exact match.

The `app_id`, `title`, and `xdg_tag` selectors take either one pattern or an array of them. Any one of them matching is enough, and an empty array places no constraint:

```toml
[[window_rule]]
match.app_id = ["^mpv$", "^celluloid$"]
window_effect = "scanlines"
```

Run `umbriel windows` to inspect current application IDs, titles, tags, and content types, or `umbriel focused-window` for the focused window only. Prefer `app_id` for placement because titles often change.

State selectors update while the window is open. Opening settings are resolved before a rule's own state changes take effect, so a rule cannot select on the floating or pinned state that it creates.

## Settings applied when a window opens

| Key                              | Description                                                                                                     |
| -------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| `default_output`                 | Open on a connector or monitor name.                                                                            |
| `default_workspace`              | Open on a one-based position or exact workspace name.                                                           |
| `default_scratchpad`             | Store in the implicit or a configured scratchpad.                                                               |
| `default_fullscreen`             | Open fullscreen.                                                                                                |
| `default_floating`               | Force floating or tiling.                                                                                       |
| `default_maximize`               | Open maximized within normal layout bounds.                                                                     |
| `default_maximize_to_edges`      | Open maximized without gaps, struts, or borders.                                                                |
| `default_focused`                | Override whether the new window takes focus. Use `true` to focus and reveal a floating window above fullscreen. |
| `default_pinned`                 | Open pinned above normal windows.                                                                               |
| `default_scrolling_column`       | Join matching windows into one named scrolling column.                                                          |
| `default_scrolling_column_order` | Set order inside a named scrolling column.                                                                      |
| `default_column_display`         | Show the column the window opens in as `"tabbed"` or `"normal"`.                                                |

These values apply once when a window opens. Umbriel checks opening rules again when the first post-map title arrives and when the effective content type first changes from `none` to a non-`none` type. This covers applications that publish their initial identity late, including Wine and Proton rendering children. Each selected opening value applies at most once across those checks, so one identity signal cannot replay it over a later compositor action. Later identity changes refresh only the settings that update while a window is open.

Named `default_workspace` targets remain valid when their output is unavailable and their workspace has been evacuated. Combining a name with `default_output` can select that original output's evacuated workspace, including when another workspace on the receiving output has the same name. Missing or ambiguous targets keep the normal placement fallback.

A `default_maximize` rule opens the window at the full column width. [`client_maximize`](layout.md#maximize-and-fullscreen) governs the client's own or restored maximize request instead, deciding whether that request lands in the edges. A rule that never sees a client request stays on the column unless `default_maximize_to_edges` is set.

Dialogs float by default. Use `default_floating = false` in a matching rule to force one into the layout.

X11 windows marked as notifications, popup menus, dropdowns, tooltips, or other client-placed popups bypass window rules, including broad application rules that force tiling or maximize. Persistent torn-off menus marked `_NET_WM_WINDOW_TYPE_MENU` remain managed and follow window rules unless they request override-redirect. If an application publishes its popup type or override-redirect flag after opening, Umbriel switches it to popup handling immediately, restores its client-requested position and size, and clears maximize and fullscreen state. These popups do not appear in `umbriel windows`.

Windows marked `_NET_WM_WINDOW_TYPE_NOTIFICATION` keep their client-requested position and size from their first visible frame, even without override-redirect, and do not take keyboard focus. They need no placement rule, including when another rule makes the application's ordinary windows floating or maximized. If an existing popup changes its type to a notification, keyboard focus returns to the active window.

Client-placed X11 popups wait for pending Xwayland geometry events before they first become visible. This prevents a queued move from briefly exposing an earlier position. Later client-requested moves continue to apply normally.

Some Wine and Proton native Wayland builds expose context menus as untitled, unparented `xdg_toplevel` windows. These receive normal window rules and have no popup anchor, so they can open centered or maximized. Hover focus can also return to the application while the pointer travels toward the menu. Such a client needs to supply a popup role and placement information. For affected applications, use the runtime's X11 driver. With Proton builds supporting both Wayland environment options, use this per-application Steam launch option:

```sh
PROTON_ENABLE_WAYLAND=0 PROTON_USE_WAYLAND=0 %command%
```

Restart the application after changing its driver. Its main window should then report `xwayland: true` in `umbriel windows --json`. The plain-text listing uses `-` as a placeholder for an empty title; matching the literal `-` does not select an untitled window. `match.title = "^$"` matches a client-supplied empty title, including these native Wayland menus, but rules cannot supply their missing popup anchor.

A modal dialog [attaches to its parent](layout.md#dialogs) and ignores the settings above, since it goes wherever its parent goes. `attach_to_parent = false` keeps it an ordinary window that these settings place. It is checked again whenever the rules change, so a reload detaches or attaches open dialogs.

A rule that matches the parent window also covers its dialogs. This is how to opt out an application whose file pickers come from the desktop portal: those dialogs carry the portal's app id, such as `xdg-desktop-portal-gtk`, not the application's. A rule that matches the dialog itself takes precedence over its parent's.

```toml
[[window_rule]]
match.app_id = "^org\\.mozilla\\.firefox$"
attach_to_parent = false
```

## Size and Position Rules

| Key                           | Applies to | Description                                                      |
| ----------------------------- | ---------- | ---------------------------------------------------------------- |
| `default_floating_size_px`    | Floating   | Logical-pixel size, such as `{ width = 800, height = 600 }`.     |
| `default_floating_size`       | Floating   | Fraction of the output usable area.                              |
| `default_scrolling_extent_px` | Scrolling  | Initial logical-pixel extent along the strip, up to 4 viewports. |
| `default_scrolling_extent`    | Scrolling  | Initial fractional extent along the strip, 0.1 to 4.0.           |
| `default_position`            | Floating   | Position from a named anchor.                                    |

Pixel values take precedence over fractions. Each axis is optional. A tiled window remembers floating size and position rules until it first floats, and a floating window remembers its scrolling extent until it first tiles.

Floating size defaults also apply when the initial title arrives between the opening configure and the first visible buffer. Placement uses the resulting size for centering and position anchors. Unchanged dimensions are not reapplied over a size the application chooses while opening. Applications can resize themselves afterward.

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

Accepted anchors are `center`, `top_left`, `top_right`, `bottom_left`, `bottom_right`, `top`, `bottom`, `left`, and `right`. Right and bottom anchors measure inward from those edges.

## Scratchpad placement

Store matching windows directly in a scratchpad:

```toml
[[scratchpad]]
name = "terminal"

[[window_rule]]
match.app_id = "^scratchpad-terminal$"
default_scratchpad = "terminal"
```

With no named definitions, use `"default"`. A hidden scratchpad keeps the new window hidden unless `default_focused = true` is also set. Output, workspace, and floating rules determine where and how it returns when restored.

The window opens with the size and position that `default_floating_size`, `default_floating_size_px`, and `default_position` give it, centered when no position is set, or opens fullscreen or maximized to the edges with `default_fullscreen` or `default_maximize_to_edges`. A window moved into a scratchpad later keeps its current size and position.

```toml
[[window_rule]]
match.app_id = "^scratchpad-terminal$"
default_scratchpad = "terminal"
default_floating_size = { width = 0.7, height = 0.7 }
```

## Workspace placement

`default_workspace` uses TOML type to distinguish positions and names:

```toml
default_workspace = 2
# default_workspace = "2"
# default_workspace = "CHAT"
```

An integer selects a one-based position. A string selects an exact, case-sensitive name. `default_output` restricts either form to one output. Names must already exist through a static inventory or a named workspace rule.

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

The name is local to a workspace. The first matching window creates the column and sets its extent.

Add `default_column_display = "tabbed"` to keep such a column as [tabs](layout.md#tab-groups): each matching window that opens tabs itself with the windows stacked beside it in the column it joins or creates.

## Settings updated while a window is open

| Key                       | Description                                                                                                                                                                                                                                                            |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `opacity`                 | Surface opacity from 0.0 to 1.0.                                                                                                                                                                                                                                       |
| `blur`                    | Enable or disable window blur, including translucent Umbriel title bars.                                                                                                                                                                                               |
| `blur_popups`             | Apply blur to descendant XDG popups.                                                                                                                                                                                                                                   |
| `blur_ignore_alpha`       | Skip blur below an alpha threshold.                                                                                                                                                                                                                                    |
| `blur_optimized`          | Override the global optimized-blur choice.                                                                                                                                                                                                                             |
| `confine_pointer`         | Keep the pointer inside this window while it has keyboard focus (default `false`).                                                                                                                                                                                     |
| `allow_shortcuts_inhibit` | Allow client-requested keyboard shortcut inhibition (default `true`). Set `false` to keep compositor shortcuts available. See [Keyboard shortcuts inhibition](#keyboard-shortcuts-inhibition).                                                                         |
| `block_capture`           | Omit this window from screenshots and recordings (default `false`). See [Capture protection](#capture-protection).                                                                                                                                                     |
| `background_render`       | Keep this window drawing while hidden (default `false`). See [Background rendering](#background-rendering).                                                                                                                                                            |
| `focus_on_activate`       | Override activation focus for this window.                                                                                                                                                                                                                             |
| `vrr`                     | Override the focused output's VRR policy.                                                                                                                                                                                                                              |
| `tearing`                 | Request or veto asynchronous presentation.                                                                                                                                                                                                                             |
| `hdr`                     | Override the focused output's HDR policy.                                                                                                                                                                                                                              |
| `border_color_focused`    | Override `colors.border.focused`.                                                                                                                                                                                                                                      |
| `border_color_unfocused`  | Override `colors.border.unfocused`.                                                                                                                                                                                                                                    |
| `border_color_outer`      | Override `colors.border.outer`.                                                                                                                                                                                                                                        |
| `border_width`            | Override `appearance.border_width`, 0 to 100.                                                                                                                                                                                                                          |
| `outer_border_width`      | Override `appearance.outer_border_width`, 0 to 100.                                                                                                                                                                                                                    |
| `title_bar`               | `"on"` follows client decoration preferences, `"negotiated"` requires Wayland decoration negotiation selecting server-side decorations, `"forced"` forces a title bar, and `"off"` hides it. Overrides `appearance.title_bar.mode`; omission inherits the global mode. |
| `title_bar_buttons`       | Title bar buttons for this window, from the title outward: any of `"minimize"`, `"maximize"`, and `"close"`, or `[]` for none. Overrides `appearance.title_bar.buttons`; omission inherits it.                                                                         |
| `corner_radius`           | Override `appearance.corner_radius`, 0 to 100.                                                                                                                                                                                                                         |
| `shadow`                  | Override `appearance.shadow.enabled`.                                                                                                                                                                                                                                  |
| `border_effect`           | Replace `effects.border` with a border preset or pool, or `"off"` to disable it.                                                                                                                                                                                       |
| `window_effect`           | Replace `effects.window` with a window preset or pool, or `"off"` to disable it.                                                                                                                                                                                       |

These values refresh when matching identity or state changes. Fullscreen bypasses rule opacity unless [`appearance.opaque_fullscreen`](appearance.md#window-appearance) is `false`.

### Focus transitions

When a window gains or loses focus, its rule-defined opacity and window and title-bar backdrop blur transition smoothly. Timing lives under [`[animation.windows_opacity]`](animation.md#window-opacity), independently of the rules defining the focused and unfocused appearances:

```toml
[animation.windows_opacity]
enabled = true
curve = "spring:1,900"

[[window_rule]]
match.is_focused = false
opacity = 0.75
blur = true

[[window_rule]]
match.is_focused = true
opacity = 1.0
blur = false
```

Enable `[appearance.blur]` for backdrop blur. Refocusing during a transition continues from the current appearance, and repeated focus requests keep its clock running. Disable `[animation.windows_opacity]` for immediate opacity and blur changes. Identity and other state changes apply their rule targets immediately, as do popup blur, decoration geometry, placement and behavior. Border colors use `[animation.border]` independently.

### Border colors

Each `border_color_*` key is independent: an unset key keeps the [`[colors.border]`](appearance.md#border-colors) color. Combined with the state selectors, they tell floating, pinned, or scratchpad windows apart:

```toml
[[window_rule]]
match.is_scratchpad = true
border_color_focused = "#E5C07BFF"
border_color_unfocused = "#5C4A2AFF"
```

### Decoration

An omitted `title_bar` inherits `[appearance.title_bar] mode`. An explicit `title_bar = "on"` follows the client's decoration preference, or shows Umbriel's bar if no preference is known. Use `"negotiated"` to show it only when Wayland decoration negotiation selects server-side decorations, `"forced"` to force a bar, or `"off"` to hide it. Both `"on"` and `"negotiated"` follow managed Xwayland windows' decoration hints. Rules override every global mode: `"forced"` can enable a bar with global `"off"`, and `"on"` can honor CSD requests with global `"forced"`. A later matching `"on"` or `"negotiated"` rule replaces an earlier override with its corresponding decoration policy. Changes apply to existing windows, including state-based rules and config reloads. Fullscreen and tiled tab groups still suppress the bar.

```toml
[[window_rule]]
match.app_id = "^my-editor$"
title_bar = "forced"

[[window_rule]]
match.app_id = "^my-editor$"
match.title = "^Preferences$"
title_bar = "on"
```

Forcing a title bar does not remove client-drawn headerbars or controls.

Keep enabled title bars available for mouse-only restore by default, or hide them only while maximized to edges:

```toml
[[window_rule]]
match.is_maximized_to_edges = true
title_bar = "off"
```

The matcher also accepts `false` and updates when an existing window enters or leaves this state, whether through a keybind, application request, or title bar.

Changing title bar presence adjusts tiled content space and maximized floating content sizes. Border-only overrides below keep layout spacing unchanged.

`border_width`, `outer_border_width`, `corner_radius`, and `shadow` change what Umbriel draws around the windows a rule matches. Layout spacing keeps using the global border widths, so a rule never moves other windows: a thinner border leaves its gap empty, and a thicker one draws into the gap. The shadow's softness, offsets, and color stay global.

An application that draws its own rounded corners and shadow looks best without Umbriel's:

```toml
[[window_rule]]
match.app_id = "^org[.]gnome[.]TextEditor$"
border_width = 0
corner_radius = 0
shadow = false
```

### Effects

```toml
[[window_rule]]
match.app_id = "^mpv$"
border_effect = "off"
window_effect = "scanlines"
```

Names refer to `[effects.preset.<name>]` or `[effects.pool.<name>]` tables of the matching kind. See [Effects](effects.md#turn-a-default-off-for-one-window-or-output).

## The only tiled window

`match.is_alone` reacts when a window becomes the only tiled window on its workspace:

```toml
[[window_rule]]
match.is_alone = true
default_maximize = true
```

It can apply fullscreen, maximize-to-edges, maximize, or a scrolling extent. The effect is removed when another tiled window appears and restored when the window becomes alone again. A window that opens with `default_pinned = true` opens floating, so it never opens in the alone state.

Combine it with other selectors when only one application should receive the behavior:

```toml
[[window_rule]]
match.is_alone = true
match.app_id = "^firefox$"
default_scrolling_extent = 0.8
```

## The only window on the workspace

`match.is_alone` counts tiled windows only, which is what fullscreen, maximize, and scrolling extents want: a float beside a window does not shrink it. `match.is_only_window` is the stricter question, and the one the [settings updated while a window is open](#settings-updated-while-a-window-is-open) ask. Nothing else may occupy the workspace, a float as much as a tile, so a tiled window and a float next to it both stop matching it:

```toml
[[window_rule]]
match.is_only_window = true
border_width = 0
outer_border_width = 0
```

A pinned window floats above the workspace rather than on it, and a scratchpad window has left it, so neither one takes the match away. The rule follows the window as company comes and goes. It does not drive the fullscreen, maximize, and extent effects above; those keep following `match.is_alone`.

## The window being shared

`match.is_cast_target` holds while a client captures the window itself, as a portal window share does, and lets go when the capture ends. Sharing a whole output does not match it. A frame that shows what is on air:

```toml
[[window_rule]]
match.is_cast_target = true
border_width = 5
border_color_focused = "#40C057FF"
border_color_unfocused = "#40C057FF"
```

The frame is not part of the captured image, so viewers never see it. A window screenshot captures the window too, so the rule also flashes on for one.

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

Effect pool choices stay with the mapped window across focus/title rule changes. Returning to a previously used pool restores its valid remembered member. Runtime effect overrides take precedence over rules; suppression preserves the underlying selection. See [Effects](effects.md#pools).

### Pointer confinement

Use `confine_pointer` for games that release their pointer lock in menus:

```toml
[[window_rule]]
match.app_id = "^steam_app_.*$"
confine_pointer = true
```

The rule applies to floating, tiled and fullscreen windows. It takes effect when that window has keyboard focus and the pointer is inside its visible content. Client-requested pointer locks still work; confinement resumes when the client releases its lock. Reloading the configuration updates existing windows, and a later matching rule can disable confinement with `confine_pointer = false`.

Use a focus or workspace keybind to escape. With focus-follows-mouse enabled, small movements over the window you just left do not refocus it; leaving and re-entering it, clicking it, or explicitly focusing it allows confinement again. Compositor cursor warps remain available. Confinement is suspended during session lock, overview, compositor move/resize and client drag operations.

### Keyboard shortcuts inhibition

Keep compositor shortcuts available in a virtual machine or remote desktop window:

```toml
[[window_rule]]
match.app_id = "^virt-manager$"
allow_shortcuts_inhibit = false
```

Use the application's ID reported by `umbriel windows --json`. The default is `true`, and a later matching rule can allow requests again with `allow_shortcuts_inhibit = true`.

This setting controls client-requested inhibition. Reloading the configuration or changing which rules match also updates existing client inhibitors. The [manual passthrough toggle](keybinds.md#keyboard-shortcuts-inhibition) remains available as an explicit override, even when the rule denies client requests. If you use the toggle to deactivate an inhibitor, rule changes preserve that choice until you toggle it again or the client creates a new inhibitor.

### Background rendering

A window that no output shows, because it sits on another workspace, in a hidden tab or in a hidden scratchpad, is told it is suspended and receives no frame callbacks, so it can stop drawing. Applications that tie other work to their drawing loop can stall or degrade in that state. Use `background_render` for them:

```toml
[[window_rule]]
match.app_id = "^obs$"
background_render = true
```

A matching window is not suspended while hidden and receives frame callbacks at its output's refresh rate, so it keeps running as if it were shown. Umbriel still does not draw it. This costs the CPU and GPU time the application spends drawing in the background. Reloading the configuration updates open windows.

### Capture protection

Keep selected windows visible locally but completely absent from screenshots and recordings:

```toml
[[window_rule]]
match.app_id = "^private-app$"
block_capture = true
```

Use the application ID from `umbriel windows --json`. Reloading updates existing windows. The last matching rule that sets `block_capture` wins; `false` removes that window's own protection. A protected parent also protects its popups and transients, even if a child has an explicit `false` rule.

Output and region captures show the content underneath, including its ongoing updates. No black rectangle replaces the window. Scrolling, dwindle, master, and stacking layouts are supported, including floating windows above tiles. A protected tile keeps its allocation; neighboring windows do not reflow. Protection does not change local visibility, input, focus, or geometry.

Direct window capture is refused. Enabling protection stops existing sessions for that window; removing it permits a fresh session. Frames already copied or submitted to a recorder cannot be recalled. Titles and application IDs remain available through existing window metadata protocols.

Protection also covers decorations, shadows, overview cards, and closing images, regardless of `effects.in_capture`. A capture that cannot be composed safely fails while local display rendering continues. Protected captures redraw the complete output and can cost more GPU time and memory.

X11 transients with a known owner inherit its protection. When an X11 popup has no reliable parent, Umbriel conservatively excludes it while any X11 window is protected; this can also hide unrelated X11 menus and notifications from capture. A protected window that changes into an unmanaged popup keeps its protection until it becomes managed again.

Raw `zwlr_export_dmabuf_manager_v1` export is not advertised because it would bypass the filtered composition. Use screencopy or image-copy capture clients instead, including image-copy clients with DMA-BUF destinations.
