# Window Rules

Window rules can match `app_id`, title, and a client-defined XDG toplevel tag
using ECMAScript regular expressions. They can also match a standardized
content type or the window's current state. Every matching rule contributes its
settings. If two rules set the same field, the rule that appears later takes
precedence. Rules from included files come before the rules in the file that
includes them.

```toml
[[window_rule]]
match.app_id = "firefox"
match.title = "^Library$"
default_floating = true
```

## Matching

| Selector | Type | Description |
|----------|------|-------------|
| `match.app_id` | regex | Match the window's app ID. |
| `match.title` | regex | Match the window's title. |
| `match.xdg_tag` | regex | Match the client-defined XDG toplevel tag. |
| `match.content_type` | string | Match `"none"`, `"photo"`, `"video"`, or `"game"`. |
| `match.is_focused` | bool | Match the window's focused state dynamically. |
| `match.is_floating` | bool | Match the window's floating state dynamically. |
| `match.is_pinned` | bool | Match the window's pinned state dynamically. |
| `match.is_scratchpad` | bool | Match the window's scratchpad state dynamically. |
| `match.is_alone` | bool | Match whether the window is the only tiled one in its workspace. |
| `match.at_startup` | bool | Match `true` during the first 60 seconds after starting umbriel and `false` afterward. |

Every selector is optional. A rule without selectors matches every window.
Regular expressions match any part of a value by default. Use `^` and `$` when
you need to match the entire value.

A selector only matches a value the client actually set. A client that never set
an app ID, a title, or a tag matches no pattern for it, while one that set an
empty string matches a pattern that accepts the empty string. `match.title =
"^$"` therefore selects windows whose title is empty, such as Firefox's browser
toolbox, and never selects windows that are simply waiting to be titled.

Run `umbriel windows` to inspect open windows. Its human-readable output adds
suffixes such as `[xdg_tag=proton-game]` and `[content_type=game]` when those
values are present. The JSON form, `umbriel windows --json`, always reports
`xdg_tag` and `content_type`, and also includes the `xwayland` boolean and the
client `pid` described in [IPC](ipc.md#queries).

An XDG toplevel tag is one client-defined string, not a fixed vocabulary. A
client can set it before the window opens and replace it later if the window's
purpose changes. The initial tag participates in opening settings. Later
replacements refresh settings from the dynamic table below, but never replay
opening settings. Windows with no tag do not match an `xdg_tag` selector; a
client that tagged itself with an empty string matches `"^$"`.

Umbriel derives a window's content type from standardized Wayland hints on its
XDG root surface and visible subsurfaces. When those hints differ, it uses the
priority `game`, `video`, `photo`, then `none`. This also covers Proton and Wine
games that publish the hint on a child surface. `none` includes windows that do
not publish a content hint. Client changes refresh settings from the dynamic
table below, but never replay the opening settings.

`is_focused`, `is_floating`, `is_pinned`, `is_scratchpad`, and `is_alone` match
the window's current state, and every one of those transitions refreshes the
settings from the dynamic table below. Pinned and scratchpad windows are
floating, so `is_floating = true` also matches them. Opening settings resolve
against the state the window opens with, before `default_floating` and
`default_pinned` apply, so a rule that sets one of those cannot also select on
the state it produces. `is_alone` selects only the four size settings described
in [The only window in the workspace](#the-only-window-in-the-workspace), and a
window that opens as the only tiled one is configured with them from its first
configure.

## Settings applied when a window opens

These settings are applied once when the window opens. Some applications set
their title shortly afterward, so Umbriel checks the rules one more time when
that first title arrives. Only newly resolved settings are applied: unchanged
opening settings do not overwrite user changes made in the meantime.

| Key | Type | Description |
|-----|------|-------------|
| `default_output` | string | Open on a specific output (e.g. `"DP-1"`). |
| `default_workspace` | int or string | Place on an existing workspace by 1-based position (1 to 64) or exact, case-sensitive name. On dynamic outputs, integer positions beyond the current count clamp to the last workspace; names never clamp. |
| `default_scratchpad` | string | Store the window in the implicit `"default"` scratchpad or an exact configured scratchpad name. Hidden scratchpads stay hidden and do not take focus. |
| `default_fullscreen` | bool | Open fullscreen across the entire output, ignoring layout struts and layer-shell exclusive zones. |
| `default_floating` | bool | Force floating (`true`) or force tiling (`false`). |
| `default_maximize` | bool | Open maximized. A tiled column still respects layout struts and gaps. Parented transient dialogs keep their natural size. |
| `default_maximize_to_edges` | bool | Explicitly open maximized to edges. The initial configure fills the usable area without layout struts, gaps, or borders, so the window does not open at its normal size first. Layer-shell exclusive zones stay visible. Takes precedence over `default_maximize`; when combined with `default_fullscreen` the window opens fullscreen and returns to maximized to edges once fullscreen is cleared. |
| `default_focused` | bool | Take focus when opening, switching to the window's workspace when needed. Defaults to `true`; set to `false` to preserve the existing focus and workspace. |
| `default_pinned` | bool | Open pinned above regular windows and keep the window visible across workspace changes. Pinning makes a tiled window floating. |
| `default_size` | `[w,h]` | Initial size in pixels, clamped to the client's min/max hints. Floats use both, then own their size and honor client resizes. A new tiled column in a horizontal scrolling layout uses the width and ignores the height; vertical scrolling, dwindle, and master use their layout geometry. Takes precedence over `default_width`/`default_height` when set. |
| `default_width` | float | Initial extent as a fraction (0.1-1.0): usable-area width for floating windows, or scrolling-axis extent for tiled windows in the scrolling layout. Overrides `layout.scrolling.default_width_fraction`; ignored by tiled windows in dwindle and master. |
| `default_height` | float | Floating windows only, on the same terms as `default_width`. Initial height as a fraction (0.1-1.0) of the usable area. Ignored for tiled windows. |
| `default_position` | table | Floating windows only, initial position: `{ x = int, y = int, anchor = string }`. Ignored for tiled windows. |
| `default_scrolling_column` | string | Scrolling windows only. Place windows with the same non-empty name in one column. Floating windows and other layout modes ignore it. |
| `default_scrolling_column_order` | int | Scrolling windows only. Position within `default_scrolling_column`, independent of launch timing. Lower values open higher in horizontal scrolling and farther left in vertical scrolling. Windows without an order follow ordered windows. |

Parented toplevels, usually dialogs, float by default even when their parent is
still opening. Set `default_floating = false` in a matching rule to force one to
tile instead.

For tiled windows, `default_maximize` expands the column to the full width
inside configured struts and gaps; for floating windows, it fills the usable
area. Client maximize requests made before the window maps are honored only when
`general.honor_restored_maximize` is enabled. Requests after mapping are always
honored.

Scrolling extents are gap-aware, so lanes whose fractions sum to `1` exactly
fill the viewport. A vertical strip applies the fraction to lane height. For a
new horizontal column, a matching `default_size` width takes
precedence over `default_width` and the configured scrolling default. Existing
named columns keep their established width. The pixel width seeds a fraction of
the opening viewport, and moving the lane within or between scrolling
workspaces retains its current fraction.

If neither `default_width` nor a matching
`layout.scrolling.default_width_fraction` is set, a scrolling window chooses
its initial logical extent.

### Workspace placement

`default_workspace` selects an existing workspace and never creates one. An
integer selects a 1-based position, while a string selects an exact,
case-sensitive name. These forms are intentionally distinct:

```toml
# Choose one form.
default_workspace = 2       # Position 2
# default_workspace = "2"   # Workspace named "2"
# default_workspace = "CHAT" # Workspace named "CHAT"
```

Action strings carry the same distinction with different syntax:
`workspace-switch:2` selects position 2, while `workspace-switch:"2"`
selects the exact name `"2"`.

On a dynamic output, an integer beyond the current workspace count selects the
last workspace. A string never clamps to another name. A name such as `"CHAT"`
can come from a static name list or from a name-based `[[workspace]]` entry that
materializes it on a dynamic output. `default_workspace` itself still selects
only an existing workspace and never creates one.

An explicit `default_output` selects the output first and scopes either form of
`default_workspace` to it. Without `default_output`, an exact name found on
exactly one live output selects that output. If several outputs contain the
same output-local name, Umbriel uses the launch output when it contains that
name. Integer positions can infer an output only when exactly one static
inventory owns that position. Otherwise Umbriel keeps the launch output and
resolves the target there. If it does not exist there, Umbriel keeps the normal
workspace placement.

## Scratchpad placement

`default_scratchpad` stores a matching window directly in a scratchpad when it
opens:

```toml
[[scratchpad]]
name = "terminal"

[[window_rule]]
match.app_id = "^scratchpad-terminal$"
default_scratchpad = "terminal"
default_output = "DP-1"
default_workspace = 2
```

With no `[[scratchpad]]` definitions, the only valid target is `"default"`.
With named definitions, the value must exactly match one of their names.
Unknown names are ignored and reported in the configuration diagnostics.

A hidden scratchpad remains hidden and the new window does not take focus. If
the selected scratchpad is already visible, the window joins it where it is
currently shown. `default_output` and `default_workspace` select the window's
saved restore destination. `default_floating` selects whether restoring it
returns it tiled or floating.

Without a scratchpad geometry override, `default_size`, `default_width`,
`default_height`, and `default_position` set the window's initial scratchpad
geometry using the assigned output's usable area.

Scratchpad presentation takes precedence over `default_pinned`,
`default_fullscreen`, `default_maximize`, and `default_maximize_to_edges`.
The enabled `animation.scratchpad` fullscreen, maximize, or scale setting also
takes precedence over opening size and position settings. A matching title
that arrives just after mapping can still select the scratchpad rule.

## Floating position

`default_position` only affects floating windows. Coordinates are logical pixels
within the output's usable area, so panels and other exclusive zones are taken
into account.

The packaged config floats browser windows titled `Picture-in-Picture` or
`Picture in picture` and places them 20 logical pixels from the bottom-right
corner. XDG shell has no semantic PiP role and does not let clients choose a
global window position, so this behavior is implemented as a title rule.

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

## Floating size

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

## Named scrolling columns

Assign the same `default_scrolling_column` to applications that should share a
scrolling column. The first matching window opens a column. Later matches on the
same workspace join it.

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

The name is local to a workspace. If a named scrolling column has been split manually,
new windows join the first column containing that name. The first window also
sets the column width. `default_scrolling_column_order` has no effect without
`default_scrolling_column`.

## Settings updated while a window is open

| Key | Type | Description |
|-----|------|-------------|
| `opacity` | float | Surface opacity (0.0-1.0). The rule is bypassed while the window is fullscreen and resumes after it leaves fullscreen; client-provided alpha remains active. With blur enabled, the translucent surface reveals a full-strength blurred backdrop, matching equivalent alpha supplied by the client. |
| `blur` | bool | Enable/disable blur for this window. |
| `blur_popups` | bool | Enable/disable blur for its XDG popups. |
| `blur_ignore_alpha` | float | Skip blur where surface alpha is below this threshold (0.0-1.0). Applies to the window and its popups. |
| `blur_optimized` | bool | Override `appearance.blur.optimized` for this window. A `true` value keeps the cached background blur alive on every output even when the global switch is off. |
| `focus_on_activate` | bool | Override `general.focus_on_activate` for activation requests targeting this window, including trusted launch tokens. `false` vetoes trusted activation focus and marks an otherwise unfocused target urgent. An untrusted request cannot suppress the window's normal `default_focused` map behavior. |
| `vrr` | string | Override the focused window's output VRR policy: `"disabled"`, `"always"`, or `"fullscreen"`. Without this key, the output's configured `vrr` policy applies. |
| `tearing` | bool | Override the client's tearing hint. Omit it to follow the hint, set `true` to request asynchronous presentation, or set `false` to veto it. The output must still opt in with `tearing = true`, and the window must be fullscreen. |
| `hdr` | string | Override the focused window's output HDR policy: `"off"`, `"on"`, `"auto"`, or `"fullscreen"`. Without this key, the output's configured `hdr` policy applies. This does not assign HDR metadata to the surface. |

## The only window in the workspace

`match.is_alone` matches when the window is the only tiled window on its
workspace. Floating windows, empty columns, and windows on other workspaces do
not count. Like focus, alone is evaluated while the window is open: opening one
more window or closing the companion flips the rules right away.

A lone window that should fill the viewport:

```toml
[[window_rule]]
match.is_alone = true
default_maximize = true
```

While the window is alone, the size-related settings below are read from the
alone rules and compared against the same settings from the window's normal
rules; the difference is applied for as long as the window stays alone. When a
second window arrives, the difference is lifted and the window returns to what
its normal rules give it.

| Setting | While alone |
|---------|-------------|
| `default_fullscreen` | The window takes the whole output. |
| `default_maximize_to_edges` | The window fills the usable area. |
| `default_maximize` | The window is maximized, unless it has a parent. |
| `default_width` | Applies to the window's scrolling lane; the layout must be scrolling. |

Only one of these is applied at a time, in the same precedence as at map time:
fullscreen, then maximized to edges, then maximized, then width. If the window
is already in the target state, the rule does not take over what the user or a
previous rule already chose.

A window that opens as the only tiled window on its workspace is configured
with these settings right away, in the same configure that carries its first
size, so its first frame is already the one the rule asks for. The rule still
owns that state: the window gives it up when a second window arrives. When the
window's normal rules set no `default_width`, the width it returns to is
`layout.scrolling.default_width_fraction`, because the alone width, not the
client's own preference, sized the window as it opened.

The rule is compatible with other matches.

```toml
[[window_rule]]
match.is_alone = true
match.app_id = "^firefox$"
default_fullscreen = true
```

A `match.is_alone = false` rule matches when the window is *not* alone, which is
handy for styling the companion windows as well. Pair the two to make a window
widen on its own and shrink beside a companion:

```toml
# Wide when alone
[[window_rule]]
match.is_alone = true
match.app_id = "^org\\.gnome\\.Nautilus$"
default_width = 0.8

# Slim when another window opens next to it
[[window_rule]]
match.is_alone = false
match.app_id = "^org\\.gnome\\.Nautilus$"
default_width = 0.4
```

The dynamic settings from the previous section can be combined with
`match.is_alone` in the same rule: they are re-applied whenever the alone state
flips, so a lone window can dim or blur itself until a companion arrives.

## Examples

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

# Disable blur for floating windows, but not scratchpad windows
[[window_rule]]
match.is_floating = true
match.is_scratchpad = false
blur = false

# Dim them even further
[[window_rule]]
match.is_focused = false
match.is_floating = true
match.is_scratchpad = false
opacity = 0.4
```
