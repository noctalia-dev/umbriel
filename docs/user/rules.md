# Window and Layer Rules

## Window Rules

Window rules can match `app_id`, title, and a client-defined XDG toplevel tag
using ECMAScript regular expressions. They can also match a standardized
content type or focus state. Every matching rule contributes its settings. If
two rules set the same field, the rule that appears later in the file takes
precedence.

```toml
[[window_rule]]
match.app_id = "firefox"
match.title = "^Library$"
default_floating = true
```

### Matching

| Selector | Type | Description |
|----------|------|-------------|
| `match.app_id` | regex | Match the window's app ID. |
| `match.title` | regex | Match the window's title. |
| `match.xdg_tag` | regex | Match the client-defined XDG toplevel tag. |
| `match.content_type` | string | Match `"none"`, `"photo"`, `"video"`, or `"game"`. |
| `match.is_focused` | bool | Match the window's focused state dynamically. |

Every selector is optional. A rule without selectors matches every window.
Regular expressions match any part of a value by default. Use `^` and `$` when
you need to match the entire value.

Run `umbriel windows` to inspect open windows. Its human-readable output adds
suffixes such as `[xdg_tag=proton-game]` and `[content_type=game]` when those
values are present. The JSON form, `umbriel windows --json`, always reports
`xdg_tag` and `content_type`, and also includes the `xwayland` boolean.

An XDG toplevel tag is one client-defined string, not a fixed vocabulary. A
client can set it before the window opens and replace it later if the window's
purpose changes. The initial tag participates in opening settings. Later
replacements refresh settings from the dynamic table below, but never replay
opening settings. Windows with no tag, or an empty tag, do not match an
`xdg_tag` selector.

Umbriel derives a window's content type from standardized Wayland hints on its
XDG root surface and visible subsurfaces. When those hints differ, it uses the
priority `game`, `video`, `photo`, then `none`. This also covers Proton and Wine
games that publish the hint on a child surface. `none` includes windows that do
not publish a content hint. Client changes refresh settings from the dynamic
table below, but never replay the opening settings.

### Settings applied when a window opens

These settings are applied once when the window opens. Some applications set
their title shortly afterward, so Umbriel checks the rules one more time when
that first title arrives. Only newly resolved settings are applied: unchanged
opening settings do not overwrite user changes made in the meantime.

| Key | Type | Description |
|-----|------|-------------|
| `default_output` | string | Open on a specific output (e.g. `"DP-1"`). |
| `default_floating` | bool | Force floating (`true`) or force tiling (`false`). |
| `default_size` | `[w, h]` | Initial size in pixels, clamped to the client's min/max hints. Floats use both, then own their size and honor client resizes; tiled windows ignore height. Takes precedence over `default_width`/`default_height` when set. |
| `default_position` | table | Initial position for floating windows: `{ x = int, y = int, anchor = string }`. Ignored for tiled windows. |
| `default_width` | float | For floating windows, the initial width as a fraction (0.1-1.0) of the usable area. This includes windows that float without `default_floating`, such as dialogs that declare a parent. For tiled windows, scrolling only: lane scroll-axis extent fraction (0.1-1.0), which is height on a vertical workspace. Gap-aware: fractions that sum to 1 tile exactly. Overrides `layout.scrolling.default_width_fraction`. Dragging the lane within or between scrolling workspaces retains its current fraction. Ignored in dwindle and master. |
| `default_height` | float | Floating windows only, on the same terms as `default_width`. Initial height as a fraction (0.1-1.0) of the usable area. Ignored for tiled windows. |
| `default_workspace` | int | Place on workspace N from 1 to 64. On dynamic outputs, values beyond the current count clamp to the last workspace. |
| `default_fullscreen` | bool | Open in fullscreen. |
| `default_maximize_to_edges` | bool | Explicitly open maximized to edges. The initial configure fills the usable area without gaps or borders, so the window does not open at its normal size first. Layer-shell exclusive zones stay visible. Takes precedence over `default_maximize`; when combined with `default_fullscreen` the window opens fullscreen and returns to maximized to edges once fullscreen is cleared. |
| `default_maximize` | bool | Explicitly open maximized. Parented transient dialogs keep their natural size. The initial configure uses the layout's final full-width target, so the window does not open at its normal size first. Umbriel ignores client maximization requested before the first buffer maps unless `general.honor_restored_maximize` is enabled, but always honors requests sent after mapping. Tiled windows expand their column to full width without changing the layout; floating windows fill the usable area. |
| `default_focused` | bool | Take focus when opening, switching to the window's workspace when needed. Defaults to `true`; set to `false` to preserve the existing focus and workspace. |
| `default_pinned` | bool | Open pinned above regular windows and keep the window visible across workspace changes. Pinning makes a tiled window floating. |

If neither `default_width` nor a matching
`layout.scrolling.default_width_fraction` is set, a scrolling window chooses
its initial logical extent.

Without `default_output`, a numbered workspace owned by exactly one fixed output
inventory also selects that output. For example, if only `DP-1` has a fourth
configured workspace, `default_workspace = 4` opens there even when the window
was launched from another output. If several fixed outputs contain that
position, Umbriel keeps the launch output. An explicit `default_output` always
scopes the workspace lookup to that output.

#### Floating position

`default_position` only affects floating windows. Coordinates are logical pixels
within the output's usable area, so panels and other exclusive zones are taken
into account.

For example, this opens a window 32 pixels right and 24 pixels up from the
bottom-left corner:

```toml
[[window_rule]]
match.app_id = "^org[.]example[.]Utility$"
default_floating = true
default_size = [800, 600]
default_position = { x = 32, y = 24, anchor = "bottom_left" }
```

Floating windows can instead be sized as fractions of the usable area, per
axis. `default_size` (pixels) wins when both are set:

```toml
[[window_rule]]
match.app_id = "^org[.]example[.]Utility$"
default_floating = true
default_width = 0.5
default_height = 0.6
```

`anchor` defaults to `"center"`, so this centers a floating window exactly:

```toml
default_position = { x = 0, y = 0 }
```

Available anchors are `"center"`, `"top_left"`, `"top_right"`,
`"bottom_left"`, `"bottom_right"`, `"top"`, `"bottom"`, `"left"`, and
`"right"`. Right anchors measure `x` leftward from the right edge; bottom
anchors measure `y` upward from the bottom edge. The single-edge anchors center
the window on the other axis. Umbriel keeps part of the window visible if an
offset would otherwise place it completely off-screen.

#### Floating size

`default_size` sizes a float in pixels. `default_width` and `default_height`
size it as fractions of the output's usable area instead, so one rule suits any
monitor. The axes are independent: an axis without a fraction keeps the size the
client asked for. Both are clamped to the client's min/max hints.

```toml
[[window_rule]]
match.app_id = "^org[.]example[.]Utility$"
default_floating = true
default_width = 0.5
default_height = 0.6
```

`default_size` wins on both axes when it is set as well.

Fractions reach every floating window the rule matches, not only windows the
rule floats with `default_floating`. A window that floats because it declares a
parent, such as a dialog, or because it fixes its size through min/max hints,
takes the fraction too. Dialogs usually share their application's `app_id`, so a
`default_width` written for scrolling lane widths also sizes that application's
dialogs. Match on `title` or `xdg_tag` to keep a rule off them.

### Settings updated while a window is open

| Key | Type | Description |
|-----|------|-------------|
| `opacity` | float | Surface opacity (0.0-1.0). The rule is bypassed while the window is fullscreen and resumes after it leaves fullscreen; client-provided alpha remains active. With blur enabled, the translucent surface reveals a full-strength blurred backdrop, matching equivalent alpha supplied by the client. |
| `blur` | bool | Enable/disable blur for this window. |
| `blur_popups` | bool | Enable/disable blur for its XDG popups. |
| `blur_ignore_alpha` | float | Skip blur where surface alpha is below this threshold (0.0-1.0). Applies to the window and its popups. |
| `blur_optimized` | bool | Override `appearance.blur.optimized` for this window. |
| `focus_on_activate` | bool | Override `general.focus_on_activate` for activation requests targeting this window, including compositor-issued `spawn:` tokens. `false` marks it urgent without focusing or switching workspaces. |
| `vrr` | string | Override the focused window's output VRR policy: `"disabled"`, `"always"`, or `"fullscreen"`. Without this key, the output's configured `vrr` policy applies. |
| `tearing` | bool | Override the client's tearing hint. Omit it to follow the hint, set `true` to request asynchronous presentation, or set `false` to veto it. The output must still opt in with `tearing = true`, and the window must be fullscreen. |
| `hdr` | string | Override the focused window's output HDR policy: `"off"`, `"on"`, `"auto"`, or `"fullscreen"`. Without this key, the output's configured `hdr` policy applies. This does not assign HDR metadata to the surface. |

### Examples

```toml
# Enable blur for every window
[[window_rule]]
blur = true
blur_optimized = true

# Narrow columns for terminals and file managers. These fractions also size any
# floating window these applications open, including their dialogs.
[[window_rule]]
match.app_id = "^(Alacritty|kitty|org\\.gnome\\.Nautilus)$"
default_width = 0.33

# Wide columns for browsers
[[window_rule]]
match.app_id = "^(helium|chromium)$"
default_width = 0.75

# Always use VRR for game content, even when the output policy disables it
[[window_rule]]
match.content_type = "game"
vrr = "always"

# Match a client-defined tag. Proton-EM uses proton-game for game windows.
[[window_rule]]
match.xdg_tag = "^proton-game$"
default_fullscreen = true

# Activate the HDR output while a matching fullscreen game is focused
[[window_rule]]
match.app_id = "^steam_app_[0-9]+$"
hdr = "fullscreen"

# Request tearing for matching fullscreen games, even without a client hint
# The output must also have tearing = true.
[[window_rule]]
match.app_id = "^(steam_app_[0-9]+|gamescope)$"
tearing = true

# Slight transparency for editors and file managers
[[window_rule]]
match.app_id = "^(code|org\\.gnome\\.Nautilus)$"
opacity = 0.97

# Float utility windows
[[window_rule]]
match.app_id = "^(Emulator|zenity|xdg-desktop-portal|qalculate-gtk|org\\.pulseaudio\\.pavucontrol)$"
default_floating = true

# Float common dialogs by title
[[window_rule]]
match.title = "^(Open File|Select|Choose a wallpaper|Open Folder|Save As|Library|Choose Where to Download|File Operation Progress|Rename|Copy Files|Move Files|Search Files)"
default_floating = true

# Games on workspace 4, fullscreen
[[window_rule]]
match.app_id = "^(steam.*|overwatch|overwatch\\.exe)$"
default_workspace = 4

[[window_rule]]
match.app_id = "^(steam_proton|steam_app.*|overwatch|overwatch\\.exe)$"
default_fullscreen = true

# Steam notification toasts
[[window_rule]]
match.title = "^notificationtoasts_.+_desktop"
default_position = { x = 0, y = 0, anchor = "bottom_right" }
default_focused = false
default_pinned = true

# Noctalia settings
[[window_rule]]
match.app_id = "^dev.noctalia.Noctalia$"
default_floating = true
default_size = [1020, 900]
blur_popups = false

# Noctalia share picker
[[window_rule]]
match.app_id = "^dev.noctalia.UmbrielSharePicker$"
default_floating = true
default_size = [800, 600]
default_position = { x = 32, y = 32, anchor = "bottom_right" }

# Swash
[[window_rule]]
match.app_id = "^dev.lemmy.swash$"
default_floating = true
default_size = [1000, 900]

# Dim unfocused windows
[[window_rule]]
match.is_focused = false
opacity = 0.85

[[window_rule]]
match.is_focused = true
opacity = 1.0
```

---

## Layer Rules

Layer rules match layer-shell surfaces such as bars, launchers, and
notifications. The `match.namespace` selector uses an ECMAScript regular
expression. Run `umbriel layers` to list the namespaces currently in use.

```toml
[[layer_rule]]
match.namespace = "^noctalia-(bar-[^\"]+|notification|dock|panel|attached-panel|osd|desktop-widget-[^\"]*)$"
blur = true
blur_ignore_alpha = 0.5
blur_popups = true
```

### Matching

| Selector | Type | Description |
|----------|------|-------------|
| `match.namespace` | regex | Match the layer surface namespace. |

Regular expressions match any part of a namespace. Use `^` and `$` to match
the entire namespace.

### Effects

| Key | Type | Description |
|-----|------|-------------|
| `blur` | bool | Enable/disable blur for the layer surface. |
| `blur_popups` | bool | Enable/disable blur for descendant XDG popups. |
| `blur_ignore_alpha` | float | Skip blur where surface alpha is below this threshold (0.0-1.0). `0.0` blurs the entire rectangle; higher values leave transparent regions unblurred. |
| `blur_optimized` | bool | Override `appearance.blur.optimized`. |

Layer-shell blur is off by default. As with window rules, every matching rule
contributes its settings, and later values take precedence.
