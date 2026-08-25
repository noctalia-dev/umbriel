# Configuration

Umbriel checks `$XDG_CONFIG_HOME/umbriel/config.toml` first, followed by each
`$XDG_CONFIG_DIRS/umbriel/config.toml`, then the packaged
`share/umbriel/config.toml`. Pass `umbriel -c <path>` to use a different file.
The packaged file is [`examples/config.toml`](../../examples/config.toml) and
can be copied into your user config directory as a starting point. Umbriel
does not create or modify a user config automatically.

## Starting configuration

Distribution packages normally install the starting configuration under
`/usr/share/umbriel/config.toml`. Copy it before making local changes:

```sh
mkdir -p ~/.config/umbriel
cp /usr/share/umbriel/config.toml ~/.config/umbriel/config.toml
```

For an installation using another prefix, replace `/usr/share` with that
installation's data directory, commonly `/usr/local/share`. Nix users should
prefer `programs.umbriel.settings` in Home Manager or hjem.

Changes normally apply as soon as you save. If a reload fails, Umbriel keeps
your last working configuration and continues watching included files. Save a
corrected file to try the reload again. Options that require a restart are
marked in the reference tables below.

## Include

```toml
[include]
files = ["appearance.toml", "keybinds.toml"]
```

Paths are resolved relative to the main config file. A leading `~` or `~/`
expands to your home directory, and `$VAR` or `${VAR}` expands environment
variables. Later files override earlier files, and values in the main file
override every include.

You can split your config into multiple files for clarity:

```toml
# ~/.config/umbriel/config.toml
[include]
files = [
  "src/general.toml",
  "src/appearance.toml",
  "src/input.toml",
  "src/keybinds.toml",
  "src/rules.toml",
  "src/workspaces.toml",
  "machines/monolith.toml",
]
```

## General

```toml
[general]
autostart = ["noctalia", "kitty"]
mod_key = "Super"
xwayland = true
show_cheatsheet = true
focus_on_activate = false
honor_restored_maximize = false
```

| Key                         | Type         | Default                 | Description                                                                                                                                                                                                                             |
| --------------------------- | ------------ | ----------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `autostart`                 | string array | `[]`                    | Shell commands run once after startup. Never re-run on config reload.                                                                                                                                                                   |
| `mod_key`                   | string       | Super (Alt when nested) | Modifier represented by `Mod` in keybinds. Accepts `Super`, `Alt`, `Ctrl`, or `Shift`; aliases `Logo`, `Win`, and `Control` are also accepted. Applies on reload.                                                                       |
| `xwayland`                  | bool         | `true`                  | Spawn `xwayland-satellite` for X11 app support. The binary must be installed. Changing this requires a restart.                                                                                                                         |
| `show_cheatsheet`           | bool         | `true`                  | Show the keybinds cheatsheet overlay on startup. If an included file is still missing, Umbriel waits for it to load before showing the overlay. Press any key or mouse button to dismiss, or toggle at runtime via `cheatsheet-toggle`. |
| `focus_on_activate`         | bool         | `false`                 | Focus and reveal windows that request activation. When false, activation marks the window and its workspace urgent without changing workspaces. Window rules can override this per application.                                         |
| `honor_restored_maximize`   | bool         | `false`                 | Honor maximized state restored by applications while their windows open. Later maximize requests are always honored. Applies to newly opened windows.                                                                                   |

## Environment

```toml
[environment]
GTK_THEME = "Adwaita:dark"
QT_QPA_PLATFORMTHEME = "qt5ct"
```

Extra environment variables exported to Umbriel and all spawned commands.
All values must be strings. Applied once at startup; changing this section
requires a restart.

## Idle inhibition

Umbriel supports application idle inhibitors and idle notifications. An
application inhibits screen blanking, locking, and other idle actions only
while its associated surface is mapped and visible. Switching away from its
workspace, hiding a scratchpad window, disabling its output, or locking the
session stops honoring that inhibitor until the surface becomes visible
again. A visible lock surface may provide its own inhibitor while the session
is locked.

## Workspaces

```toml
[workspaces]
back_and_forth = true
```

| Key              | Type | Default | Description                                                                                     |
| ---------------- | ---- | ------- | ----------------------------------------------------------------------------------------------- |
| `back_and_forth` | bool | `false` | Re-selecting the active workspace jumps back to the previously active workspace on that output. |

Output workspaces are dynamic by default. See [Outputs](outputs.md) for dynamic
behavior and fixed workspace lists. See
[Workspace rules](outputs.md#workspace-rules) for per-workspace layout
overrides.

## Colors

```toml
[colors]
background = "#141419F0"
text_primary = "#E8E8EAFF"
text_muted = "#8A8A92FF"
accent_primary = "#7AA3FFFF"
accent_secondary = "#F5C96BFF"
warning = "#F5C96BFF"
error = "#FF6B6BFF"
```

Shared semantic colors for Umbriel-owned interface surfaces such as the keybind
cheatsheet and configuration diagnostic banner. Colors are `#RRGGBB` or
`#RRGGBBAA`.

| Key                | Type  | Default     | Description                                        |
| ------------------ | ----- | ----------- | -------------------------------------------------- |
| `background`       | color | `#141419F0` | Shared background for internal panels and banners. |
| `text_primary`     | color | `#E8E8EAFF` | Primary text.                                      |
| `text_muted`       | color | `#8A8A92FF` | Secondary help and status text.                    |
| `accent_primary`   | color | `#7AA3FFFF` | Primary emphasis, including titles and key chords. |
| `accent_secondary` | color | `#F5C96BFF` | Secondary emphasis, including group headings.      |
| `warning`          | color | `#F5C96BFF` | Warning status text.                               |
| `error`            | color | `#FF6B6BFF` | Error status text.                                 |

Key chord backgrounds are derived from `background` and `text_primary`; they
remain opaque so text stays legible over translucent panels.

## Appearance

```toml
[appearance]
prefer_no_csd = true
border_width = 2               # 0-100
outer_border_width = 0         # 0-100
corner_radius = 10             # 0-100, 0 disables
border_focused = "#7AA3FFFF"   # #RRGGBB or #RRGGBBAA
border_unfocused = "#292933FF"
scratchpad_border_focused = "#E5C07BFF"
scratchpad_border_unfocused = "#5C4A2AFF"
outer_border_color = "#1A1A1FFF"
insert_hint_color = "#7FC8FF80"
backdrop_color = "#000000FF"
animation_ms = 200             # 1-10000
drag_opacity = 0.75
```

| Key                           | Type  | Default     | Description                                                                                                                                       |
| ----------------------------- | ----- | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `prefer_no_csd`               | bool  | `true`      | Ask clients to omit client-side decorations (xdg-decoration). Clients that explicitly request CSD are still honored. Restart apps after changing. |
| `border_width`                | int   | `2`         | Inner border width in logical pixels (0-100), including around rounded corners.                                                                   |
| `outer_border_width`          | int   | `0`         | Ring outside the inner border in logical pixels (0-100).                                                                                          |
| `corner_radius`               | int   | `10`        | Rounded corner radius (0-100). 0 disables.                                                                                                        |
| `border_focused`              | color | `#7AA3FFFF` | Border color for the focused window.                                                                                                              |
| `border_unfocused`            | color | `#292933FF` | Border color for unfocused windows.                                                                                                               |
| `scratchpad_border_focused`   | color | `#E5C07BFF` | Border color for the focused scratchpad window.                                                                                                   |
| `scratchpad_border_unfocused` | color | `#5C4A2AFF` | Border color for unfocused scratchpad windows.                                                                                                    |
| `outer_border_color`          | color | `#1A1A1FFF` | Outer border color (no focus variant).                                                                                                            |
| `insert_hint_color`           | color | `#7FC8FF80` | Drop-target preview during drag.                                                                                                                  |
| `backdrop_color`              | color | `#000000FF` | Background for fullscreen gaps and lock screen.                                                                                                   |
| `animation_ms`                | int   | `200`       | Animation duration in milliseconds (1-10000).                                                                                                     |
| `drag_opacity`                | float | `0.75`      | Opacity of the window while dragging.                                                                                                             |

Colors are `#RRGGBB` or `#RRGGBBAA`.

See [Scratchpads](scratchpad.md) for how scratchpad windows behave and use the
dedicated border colors.

### Blur

```toml
[appearance.blur]
enabled = true
optimized = true
passes = 3        # 0-8
radius = 5        # 0-100
noise = 0.02      # 0.0-1.0
brightness = 0.9  # 0.0-2.0
contrast = 0.9    # 0.0-2.0
saturation = 1.1  # 0.0-2.0
```

`enabled` is the master switch. Individual surfaces must still opt in through
[window rules](rules.md) or [layer rules](rules.md#layer-rules).
Blur only renders where a surface is transparent. Sampling remains confined to
the surface's owning output when a window overflows into a neighbouring output.
Disabling the master switch also releases SceneFX's per-output blur render
targets.

| Key          | Type  | Default | Description                                                              |
| ------------ | ----- | ------- | ------------------------------------------------------------------------ |
| `enabled`    | bool  | `true`  | Master blur switch.                                                      |
| `optimized`  | bool  | `true`  | Cache one background blur per output instead of recomputing per surface. |
| `passes`     | int   | `3`     | Blur passes (0-8). 0 disables.                                           |
| `radius`     | int   | `5`     | Blur radius (0-100). 0 disables.                                         |
| `noise`      | float | `0.02`  | Noise overlay (0.0-1.0).                                                 |
| `brightness` | float | `0.9`   | Brightness adjustment (0.0-2.0).                                         |
| `contrast`   | float | `0.9`   | Contrast adjustment (0.0-2.0).                                           |
| `saturation` | float | `1.1`   | Saturation adjustment (0.0-2.0).                                         |

### Shadow

```toml
[appearance.shadow]
enabled = true
softness = 10      # 0-200
offset_x = 2       # -200 to 200
offset_y = 2
color = "#0000007F"
```

Drop shadow behind windows (tiled and floating). Hidden while fullscreen.

| Key        | Type  | Default     | Description                                                            |
| ---------- | ----- | ----------- | ---------------------------------------------------------------------- |
| `enabled`  | bool  | `true`      | Enable drop shadows.                                                   |
| `softness` | int   | `10`        | Gaussian blur sigma in pixels (0-200). 0 produces a hard-edged shadow. |
| `offset_x` | int   | `2`         | Horizontal shadow offset (-200 to 200).                                |
| `offset_y` | int   | `2`         | Vertical shadow offset (-200 to 200).                                  |
| `color`    | color | `#0000007F` | Shadow color.                                                          |

### Animations

Per-event duration, curve, and (where applicable) style, on top of the
top-level `animation_ms` fallback. Values below are the built-in defaults.

```toml
[appearance.animations]
enabled = true

[appearance.animations.windows_in]
enabled = true
duration_ms = 220
curve = "snappy"
style = "popin"       # "popin", "zoom", "slide", "fade", "none"

[appearance.animations.windows_out]
enabled = true
duration_ms = 200
curve = "snappy"
style = "popin"       # "popout", "zoom", "slide", or falls through to a plain fade

[appearance.animations.windows_move]
enabled = true
duration_ms = 200
curve = "snappy"

[appearance.animations.workspaces]
enabled = true
duration_ms = 250
curve = "snappy"
style = "slide"        # "slide" or "fade"

[appearance.animations.scratchpad]
enabled = true
duration_ms = 250
curve = "snappy"
dim = 0.2               # 0.0-1.0: dims the rest of the output while a scratchpad window is shown

[appearance.animations.border]
enabled = true
duration_ms = 200
curve = "snappy"

[appearance.animations.dim_unfocused]
enabled = true
duration_ms = 200
curve = "snappy"
dim = 0.0               # 0.0-1.0

[appearance.animations.fade]
enabled = true
duration_ms = 200
curve = "snappy"
```

| Key                            | Type   | Default   | Description                                                                    |
| ------------------------------- | ------ | --------- | -------------------------------------------------------------------------------- |
| `enabled`                       | bool   | `true`    | Master switch for the per-event overrides below. When off, everything falls back to `appearance.animation_ms`/`animation_curve`. |
| `windows_in.*`                  |        |           | Window open.                                                                    |
| `windows_out.*`                 |        |           | Window close (plays on a scene-tree snapshot of the closing window).            |
| `windows_move.*`                |        |           | Move/resize animation, including maximize/tile transitions.                     |
| `workspaces.*`                  |        |           | Workspace switch. `style = "fade"` matches Hyprland's stock default; `"slide"` is the umbriel default. |
| `scratchpad.*`                  |        |           | Scratchpad (special workspace) show/hide. Always a plain fade in place — the window is never resized or centered, matching Hyprland. `dim` controls the background dim behind it. |
| `border.*`                      |        |           | Focus-ring color transition (interpolated in OkLab color space).                |
| `dim_unfocused.*`                | | | Opacity dim applied to unfocused windows; `dim` is the dim amount (0 = off). |
| `fade.*`                        |        |           | Generic fade used where no more specific category applies (e.g. layer-shell surfaces). |

Each `*.curve` accepts a named preset (`linear`, `ease`, `easeout`, `snappy`,
`bounce`, `elastic`, ... — see below), a raw bezier as `"x1,y1,x2,y2"`, or a
spring as `"spring: damping,stiffness"`.

Custom named curves can be registered once and reused by name:

```toml
[appearance.animations.beziers]
myBezier = [0.05, 0.9, 0.1, 1.05]

[appearance.animations.springs]
myBounce = { damping = 0.5, stiffness = 200 }
```

Then reference them as `curve = "myBezier"` or `curve = "myBounce"` in any of
the sections above.

## Overview

```toml
[overview]
zoom = 0.5                     # 0.1-0.75
background_blur = true
background_tint = "#10101430"
workspace_background = "#00000044"
```

The wallpaper is blurred while the overview is open using the `[appearance.blur]`
parameters. Set `background_blur = false`, or disable appearance blur, to turn it
off.

### Open and navigate

The overview shows every workspace on every output. Press `Mod+O` by default,
or use one of the [overview actions](keybinds.md#overview-actions).

Click a window to focus it, middle-click to close it, or drag it to another
workspace. When a click selects a window in another scrolling column, the
column reveal runs together with the closing zoom. Use the wheel, arrow keys,
or a 3-finger swipe to move through the workspace list. While the overview is
open, each gesture moves one workspace at a time. A 4-finger swipe opens or
closes the overview.

An active client drag takes precedence. Umbriel ignores requests to open the
overview until the pointer button that initiated the drag is released.

### Move windows

Dragged windows become translucent so you can see the destination beneath
them. In the dwindle layout, the preview shows the direction of the new split
before you drop the window.

### Appearance

Overview cards use the same borders, corner radius, transparency, and blur as
their windows. `workspace_background` adds a rounded background behind each
workspace. Its alpha can produce anything from a light tint to an opaque fill.

| Key                    | Type  | Default     | Description                                                                                    |
| ---------------------- | ----- | ----------- | ---------------------------------------------------------------------------------------------- |
| `zoom`                 | float | `0.5`       | Workspace scale when fully zoomed out (0.1-0.75).                                              |
| `background_blur`      | bool  | `true`      | Blur the wallpaper behind the filmstrip. Uses the `[appearance.blur]` parameters.             |
| `background_tint`      | color | `#10101430` | Tint composited over the desktop background. Alpha `00` leaves it untouched; `FF` hides it.    |
| `workspace_background` | color | `#00000044` | Rounded background behind each workspace. Alpha `00` makes it invisible; `FF` makes it opaque. |

## Hot corners

```toml
[hot_corners.top_left]
enabled = true
delay_ms = 500
action = "overview-open"

[hot_corners.bottom_right]
enabled = true
delay_ms = 750
action = "spawn:notify-send 'Bottom right'"
```

Each corner has its own enabled state, delay, and action. Actions use the same syntax as
keybind values. Omitted corners do nothing, and `enabled = false` disables a corner without
removing its action. A delay of `0` activates immediately.
Hot corners are inactive on an output while a window is fullscreen there.

Available subsections are `hot_corners.top_left`, `hot_corners.top_right`,
`hot_corners.bottom_left`, and `hot_corners.bottom_right`.

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `enabled` | bool | `false` | Enable this corner. |
| `delay_ms` | int | `500` | Time at this corner before its action runs (0-10000). |
| `action` | string | unset | Keybind-style action to run. |

## Layout

```toml
[layout]
mode = "scrolling"                  # "scrolling" or "dwindle"
gap = 8                             # 0-500
width_presets = [0.333, 0.5, 0.667]

[layout.scrolling]
direction = "horizontal"             # "horizontal" or "vertical"
default_width_fraction = 0.5         # remove to let clients choose, 0.1-1.0
center_underfull_strip = true
```

Shared layout options:

| Key             | Type        | Default               | Description                                                        |
| --------------- | ----------- | --------------------- | ------------------------------------------------------------------ |
| `mode`          | string      | `"scrolling"`         | Layout algorithm: `"scrolling"` or `"dwindle"`.                    |
| `gap`           | int         | `8`                   | Gap between windows in pixels (0-500).                             |
| `width_presets` | float array | `[0.333, 0.5, 0.667]` | Widths visited by the `window-cycle-width` action in both layouts. |

Scrolling layout options:

| Key                      | Type   | Default        | Description                                                                                                                       |
| ------------------------ | ------ | -------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| `direction`              | string | `"horizontal"` | Scroll axis: `"horizontal"` stacks columns left to right; `"vertical"` stacks lanes top to bottom.                                |
| `default_width_fraction` | float  | unset          | Initial scroll-axis extent assigned to new scrolling lanes (0.1-1.0). The packaged config sets `0.5`; when omitted, the client chooses its initial extent. |
| `center_underfull_strip` | bool   | `true`         | Center the complete strip whenever it is shorter than the viewport. Disable to align it at the start edge.                        |

On a vertical scrolling workspace, each column becomes a horizontal lane. Lanes
stack from top to bottom, and windows within a lane sit side by side. Existing
width vocabulary, including `default_width_fraction`, `width_presets`,
`window-cycle-width`, `window-set-width`, `window-modify-width`, and
`window-toggle-maximize`, controls the lane's extent along the scroll axis. In
other words, it controls lane height on a vertical workspace.

The packaged config sets `default_width_fraction = 0.5` so new scrolling lanes
start at half the viewport. When the option is removed, Umbriel leaves the
scroll-axis dimension unconstrained in the initial configure and retains the
logical size chosen by the client. A numeric window-rule `default_width` still
takes precedence for matching applications.

Directional focus and movement follow the screen: left and right operate within
a vertical lane, while up and down walk or reorder lanes along the strip.
`window-consume-left` still merges into the previous lane, which is visually
above, and `window-expel-right` creates the next lane, which is visually below.
The three-finger vertical swipe continues to switch workspaces. The
three-finger horizontal strip gesture is inert on vertical workspaces, so use
keyboard or wheel bindings to scroll the strip.

Mod+Right-drag selects horizontal and vertical resize edges from the outer
thirds of both tiled and floating windows. Dragging from a corner region resizes
both axes. Mod+Right-click in the center region starts no resize and preserves
the window's maximize state. For tiled windows, a center click also scrolls the
focused window into view. When a tiled resize ends, the focused scrolling column
animates back into view. In the dwindle layout, only edges backed by an internal
split propose a resize, so screen-facing edges propose nothing.

When focus moves to a partially or fully hidden column, Umbriel scrolls by the
shortest distance needed to reveal it completely. A column entering from the
right aligns with the right viewport edge, and a column entering from the left
aligns with the left edge. Focus never reserves a visible sliver for the next
column.

Resizing a column recenters an underfull strip immediately.

Dragged windows become translucent so the insertion preview remains visible.
Existing window transparency still applies during the drag.
When you drag a column, the preview uses the free space beside the real column
edges. If the strip extends beyond the output, its far left and right edges
remain visible prepend and append targets, even when the corresponding end
columns are off-screen.

Dropping a window into empty space above or below a vertically resized stack
consumes that space. Existing windows retain their pixel heights, and the
dropped window fills the remainder apart from the configured inter-window gap.

In the dwindle layout, a new window splits an existing one along that window's
longer edge, so a landscape monitor starts side by side and a portrait monitor
starts stacked. The direction is fixed when the split is created: resizing one
boundary never reorients another split. Dropping a window on a specific edge
picks that direction explicitly instead.

Layout fields can be overridden per-workspace; see
[Workspace Rules](outputs.md#workspace-rules).

## Input

```toml
[input]
middle_click_paste = false
```

`middle_click_paste` controls the primary-selection clipboard. It defaults to
`true`. Set it to `false` to disable pasting selected text with a middle click
from either a mouse or touchpad. This also disables other primary-selection
paste methods such as Shift+Insert, while the regular clipboard used by Ctrl+C
and Ctrl+V remains available.

When disabled, Umbriel clears the current primary selection and rejects new
primary selections from connected clients. Applications started while it is
disabled are not offered the primary-selection protocol. The setting applies
immediately on config reload. Applications started while it was disabled must
be restarted after re-enabling it.

### Keyboard

```toml
[input.keyboard]
layout = ""       # XKB layout, empty = system default
variant = ""      # XKB variant
options = ""      # XKB options, comma-separated
repeat_rate = 25  # 0-1000 Hz, 0 disables
repeat_delay = 600 # 0-10000 ms
numlock_toggle = true # true enables NumLock when a keyboard connects; false leaves it off
```

`layout` takes a comma-separated list to load several layouts at once
(`layout = "us,de"`, optionally with a matching `variant = ",nodeadkeys"`). The
first entry is active at startup. Switch between them with the
`keyboard-layout-next` keybind or `umbriel msg keyboard-layout-next`, or put a
toggle in `options`:

```toml
[input.keyboard]
layout = "us,de"
options = "grp:alt_shift_toggle"
```

`options` is passed to XKB verbatim, so anything `xkbcli list` reports under
options works (`grp:win_space_toggle`, `caps:escape`, `compose:ralt`, …). An
`options` value XKB does not recognize is ignored silently, the same as with
`setxkbmap`; a `layout` or `variant` that fails to compile is reported in the
log and the whole keyboard block falls back to the system default.

### Touchpad

```toml
[input.touchpad]
tap = true
natural_scroll = true
# accel_profile = "adaptive"  # "flat", "adaptive", or a custom curve
# sensitivity = 0.5           # -1.0 to 1.0
```

Tap-to-click is enabled by default. Set `tap = false` to disable it globally,
or use a per-device override below. `natural_scroll` remains unset by default,
which preserves each device's libinput setting. Options are applied only when
supported by the device.

`accel_profile` and `sensitivity` work like their `[input.mouse]` counterparts,
including custom curves. Both remain unset by default, which uses each
touchpad's libinput default profile and speed. Removing either setting on reload
restores the corresponding default. `sensitivity` alone adjusts pointer speed
under the device's default profile.

### Mouse

```toml
[input.mouse]
natural_scroll = false
# accel_profile = "flat"  # "flat", "adaptive", or a custom curve
sensitivity = 0.0        # -1.0 to 1.0
scroll_wheel_step = 60  # 1-1000, pixels per step for layout-scroll-left/right
```

Omitting `accel_profile` preserves each device's libinput default, which is
usually `adaptive` for a mouse. Set `accel_profile = "flat"` to disable
speed-dependent acceleration, or set it to `adaptive` explicitly to override a
different device default. `sensitivity` controls pointer speed independently of
the selected profile. A custom curve can be supplied with this syntax:

```toml
accel_profile = "custom 0.2 0.0 0.5 1.0 2.0"
```

The first number is the positive input-speed step, followed by at least two
non-negative output-speed points. Libinput interpolates between them.
`sensitivity` has no effect when a custom profile is selected. Omit
`natural_scroll` or `accel_profile` to preserve each device's corresponding
libinput default. `layout-scroll-left` and `layout-scroll-right` clamp to the
strip bounds, so the columns never park
past either edge. Wheel-triggered scrolling uses twice `scroll_wheel_step`
during an active tiled window drag.

### Per-device overrides

Use `[[input.device]]` to override settings for devices whose name exactly
matches `name`. Matching is case-sensitive. The name is the `Device` value
reported by `libinput list-devices`.

```toml
[[input.device]]
name = "Acme Split Keyboard"
layout = "us"
variant = "colemak_dh"
repeat_rate = 40
repeat_delay = 250

[[input.device]]
name = "Acme Precision Touchpad"
tap = true
natural_scroll = false
accel_profile = "flat"
sensitivity = 0.0

[[input.device]]
name = "Acme Gaming Mouse"
accel_profile = "flat"
sensitivity = 0.0
```

Each rule inherits the matching class settings and overrides only the keys it
contains. `layout`, `variant`, `options`, `repeat_rate`, and `repeat_delay`
apply to keyboards. `tap` applies to touchpads. `natural_scroll` applies to
touchpads and mice. `accel_profile` and `sensitivity` apply to mice and
touchpads; for a touchpad the rule overrides `[input.touchpad]` rather than
`[input.mouse]`. Unsupported libinput settings are reported in the log.

Rules match every attached device with the exact name. Device overrides also
apply when a device is connected after startup and when the configuration is
reloaded. Duplicate rules for the same name are rejected.

`scroll_wheel_step`, cursor settings, tablet settings, and focus settings remain
compositor-wide because they are not properties of one physical input device.

### Tablet

```toml
[input.tablet]
enabled = true                 # false disables the tablet and its pads
map_to_output = "DP-1"         # confine the tablet area to one monitor
map_to_focused_output = false
map_to_focused_window = false  # pen area = focused window
left_handed = false
calibration_matrix = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0]  # libinput calibration, 6 numbers
```

Stylus and pad input is delivered over the tablet-v2 protocol to clients that
support it (pressure, tilt, eraser as a distinct tool, pad buttons, rings, and
strips). Every other client receives pointer emulation instead: the tip acts as
the left button, `BTN_STYLUS` as the right button, and `BTN_STYLUS2` as the
middle button.

| Key                      | Type  | Default | Description                                                                                                        |
| ------------------------ | ----- | ------- | ------------------------------------------------------------------------------------------------------------------ |
| `enabled`                | bool  | `true`  | Silences the tablet and its pads at the libinput level. Has no effect on devices libinput cannot disable.          |
| `map_to_output`          | str   | (none)  | Confines the tablet area to the named output, using the same names as `[output.NAME]`.                             |
| `map_to_focused_output`  | bool  | `false` | Pen area follows the output holding keyboard focus.                                                                |
| `map_to_focused_window`  | bool  | `false` | Pen area tracks the focused window.                                                                                |
| `left_handed`            | bool  | `false` | Flips the tablet orientation via libinput.                                                                         |
| `calibration_matrix`     | array | (none)  | Six finite numbers passed to libinput; omitting the key restores the device default.                               |

The mapping options form a cascade. `map_to_focused_window` wins while a window
is focused; otherwise `map_to_focused_output` applies while an output holds
keyboard focus; otherwise `map_to_output` applies while that output is
connected; otherwise the pen covers the full output layout. Each level falls
through to the next when its target is unavailable, so combining options is
harmless. The tablet area is stretched to the target box without aspect-ratio
correction. `enabled`, `left_handed`, and `calibration_matrix` changes apply on
config reload, as do the mapping options for the next pen event.

### Cursor

```toml
[input.cursor]
theme = ""   # empty = environment/default Xcursor theme
size = 24    # 1-512
hardware_cursor = true
hide_when_typing = false
hide_timeout_ms = 0  # 0-3600000, 0 disables hiding
```

Set `hardware_cursor = false` to composite the cursor in the output render pass.
This can work around cursor flicker or disappearance caused by hardware cursor
planes. Cursor settings apply on config reload. Output scale changes also reload
the cursor image at the matching scale without requiring a restart.
Set `hide_when_typing = true` to hide the cursor immediately after a
non-modifier key press. Modifier-only presses leave it visible.
Set `hide_timeout_ms` to a value from `1` to `3600000` to hide the cursor after
that many milliseconds without pointer activity. Motion, clicks, scrolling,
and tablet input reveal the cursor and restart the timeout. The two hiding
options can be enabled together.

### Focus

```toml
[input.focus]
follows_mouse = false
follows_mouse_max_scroll = 0.5  # optional, measured in viewport widths
```

| Key                        | Type  | Default    | Description                                                                                                                                                                     |
| -------------------------- | ----- | ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `follows_mouse`            | bool  | `false`    | Focus a window when the pointer enters it, then scroll it into view.                                                                                                            |
| `follows_mouse_max_scroll` | float | (no limit) | Do not change focus when revealing the window would scroll farther than this many viewport widths. `0.0` allows only windows that are already fully visible. Omit for no limit. |

For example, a window three screens away requires a limit of at least `3.0`.
Values outside `0.0` to `100.0` are clamped and reported.
