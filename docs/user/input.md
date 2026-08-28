# Input

This page covers compositor-wide input settings and per-device behavior.

## Settings

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
# scroll_factor = 1.5         # touchpad scroll speed, 0.1 to 10.0
# disable_while_typing = true
```

Tap-to-click is enabled by default. Set `tap = false` to disable it globally,
or use a per-device override below. `natural_scroll` and
`disable_while_typing` remain unset by default, which preserves each device's
corresponding libinput default. Set `disable_while_typing = false` to keep the
touchpad active while typing. Removing either optional setting on reload
restores the device default. Options are applied only when supported by the
device; an explicitly configured unsupported option is reported in the log.

`accel_profile` and `sensitivity` work like their `[input.mouse]` counterparts,
including custom curves. Both remain unset by default, which uses each
touchpad's libinput default profile and speed. Removing either setting on reload
restores the corresponding default. `sensitivity` alone adjusts pointer speed
under the device's default profile.

`scroll_factor` multiplies the smooth two-finger scroll a touchpad sends to the
focused window, so `2.0` scrolls twice as fast and `0.5` half as fast. It
remains unset by default (identity, `1.0`) and takes the next scroll event on
reload. It applies only to the continuous scroll delta: discrete notches,
overview wheel stepping, and three-finger-swipe strip travel keep their own
counting semantics.

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
disable_while_typing = false

[[input.device]]
name = "Acme Gaming Mouse"
accel_profile = "flat"
sensitivity = 0.0
```

Each rule inherits the matching class settings and overrides only the keys it
contains. `layout`, `variant`, `options`, `repeat_rate`, and `repeat_delay`
apply to keyboards. `tap` and `disable_while_typing` apply to touchpads.
`natural_scroll` applies to touchpads and mice. `accel_profile` and
`sensitivity` apply to mice and touchpads; for a touchpad the rule overrides
`[input.touchpad]` rather than `[input.mouse]`. Unsupported libinput settings
are reported in the log.

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
follows_focus = false
hide_when_typing = false
hide_timeout_ms = 0  # 0-3600000, 0 disables hiding
```

Set `hardware_cursor = false` to composite the cursor in the output render pass.
This can work around cursor flicker or disappearance caused by hardware cursor
planes. Cursor settings apply on config reload. Output scale changes also reload
the cursor image at the matching scale without requiring a restart.
Set `hide_when_typing = true` to hide the cursor after a non-modifier key
press. Modifier-only presses leave it visible. Typing while a pointer button is
held also leaves it visible so active clicks, drags, and game actions are not
interrupted.
Set `hide_timeout_ms` to a value from `1` to `3600000` to hide the cursor after
that many milliseconds without pointer activity. Motion, clicks, scrolling,
and tablet input reveal the cursor and restart the timeout. The two hiding
options can be enabled together.

Set `follows_focus = true` to warp the cursor to the visible center of a window
selected by directional window focus, next-window focus, floating-state focus,
or first/last-column focus navigation. This applies whether the action comes
from a keybind, wheel bind, or IPC. Pointer-driven focus, automatic focus after
a window closes, gestures, and overview selection do not warp the cursor.
`window-focus:<id>` remains focus-only; use `window-focus-warp:<id>` when an
individual id-based request must always move the cursor.

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
