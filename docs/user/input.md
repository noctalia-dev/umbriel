# Input

Configure keyboard, pointer, touchpad, tablet, cursor, and focus behavior under
`[input]`.

## Settings

```toml
[input]
middle_click_paste = false
window_drag_toggle = "none"
```

`middle_click_paste = false` disables primary-selection paste, including
Shift+Insert. The regular Ctrl+C and Ctrl+V clipboard is unaffected.
Applications started while primary selection is disabled must be restarted
after it is re-enabled.

`window_drag_toggle` controls what pressing the other main mouse button does
during a window drag:

| Value | Behavior |
| --- | --- |
| `"none"` | Leave the drag unchanged. |
| `"floating"` | Toggle whether the dropped window is tiled or floating. |
| `"pinned"` | Toggle whether the dropped window is pinned. |

The state changes when the window is dropped. Unsupported transitions, such as
pinning a fullscreen window, leave the window unchanged.

### Keyboard

```toml
[input.keyboard]
layout = ""           # empty uses the system default
variant = ""
options = ""
repeat_rate = 25
repeat_delay = 600
numlock_toggle = true
track_layout = "global"
```

| Key | Range or values | Description |
| --- | --- | --- |
| `repeat_rate` | 0 to 1000 Hz | Key repeats per second; `0` disables repeat. |
| `repeat_delay` | 0 to 10000 ms | Delay before a held key repeats. |
| `numlock_toggle` | bool | Enable NumLock when a keyboard connects. |
| `track_layout` | `"global"` or `"window"` | Choose session-wide or per-window layout tracking. |

Use a comma-separated layout list to configure several layouts:

```toml
[input.keyboard]
layout = "us,de"
options = "grp:alt_shift_toggle"
```

`options` accepts XKB options such as `caps:escape` or `compose:ralt`. Invalid
layouts and variants are reported in the log and fall back to the system
default. Run `xkbcli list` to inspect available values.

Run `umbriel keyboard-layouts` to list the configured layouts. The active one
is prefixed with `*`.

#### Layout switching

Bind `keyboard-layout-next` to cycle configured physical keyboard layouts:

```toml
[keybinds]
"Mod+Shift+K" = "keyboard-layout-next"
```

`umbriel msg keyboard-layout-next` provides the same action for scripts.
Physical keyboards that provide the selected layout stay synchronized. Virtual
keyboards retain their application-provided keymap.

#### Tracking the layout per window

`track_layout` accepts:

| Value | Behavior |
| --- | --- |
| `"global"` | Use one active layout for the session. |
| `"window"` | Remember the active layout for each focused surface. |

In window mode, a surface that has not been focused before starts with the
first configured layout. Reloading keyboard configuration clears remembered
surface layouts.

### Touchpad

```toml
[input.touchpad]
tap = true
natural_scroll = true
# accel_profile = "adaptive"
# sensitivity = 0.5
# scroll_factor = 1.5
# disable_while_typing = true
# disable_on_external_mouse = true
# click_method = "clickfinger"
```

Omitted values preserve the device's libinput defaults. Explicit unsupported
settings are reported in the log.

| Key | Description |
| --- | --- |
| `tap` | Enable tap-to-click. |
| `natural_scroll` | Reverse scrolling and three-finger gesture direction. |
| `accel_profile` | Use `"flat"`, `"adaptive"`, or a custom acceleration curve. |
| `sensitivity` | Pointer speed from -1.0 to 1.0. |
| `scroll_factor` | Application scroll multiplier from 0.1 to 10.0. |
| `disable_while_typing` | Disable the touchpad during keyboard input. |
| `disable_on_external_mouse` | Disable the touchpad while an external mouse is connected. |
| `click_method` | Use `"button_areas"` or `"clickfinger"`. |

`scroll_factor` also accepts per-axis values:

```toml
scroll_factor = { horizontal = 2.0, vertical = 1.5 }
```

This factor changes continuous two-finger application scrolling. Overview
navigation uses the factors documented in
[Workspaces Overview](workspaces-overview.md#settings-and-behavior).

### Mouse

```toml
[input.mouse]
natural_scroll = false
# accel_profile = "flat"
sensitivity = 0.0
scroll_wheel_step = 60
# scroll_button = "MouseBack"
# scroll_button_lock = false
```

`sensitivity` ranges from -1.0 to 1.0. `scroll_wheel_step` accepts 1 to 1000
logical pixels per layout-scroll action.

Omitting `accel_profile` preserves the device default. A custom libinput curve
uses this form:

```toml
accel_profile = "custom 0.2 0.0 0.5 1.0 2.0"
```

`scroll_button` turns pointer motion into scrolling while the named button is
held. Set `scroll_button_lock = true` to toggle that mode with a press instead.
Accepted names are `MouseLeft`, `MouseRight`, `MouseMiddle`, `MouseBack`, and
`MouseForward`.

`scroll_wheel_step` controls the distance used by layout scroll actions, not
the scrolling sent to applications.

### Per-device overrides

Use `[[input.device]]` for a device whose case-sensitive name exactly matches
the `Device` value from `libinput list-devices`:

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
click_method = "clickfinger"

[[input.device]]
name = "Acme Gaming Mouse"
accel_profile = "flat"
sensitivity = 0.0
```

Each rule inherits its keyboard, touchpad, or mouse class settings and replaces
only the keys it contains. Duplicate rules for the same device name are
rejected.

### Tablet

```toml
[input.tablet]
enabled = true
map_to_output = "DP-1"
map_to_focused_output = false
map_to_focused_window = false
left_handed = false
calibration_matrix = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0]
```

| Key | Description |
| --- | --- |
| `enabled` | Enable tablet and pad input. |
| `map_to_output` | Confine the tablet to a connector or monitor name. |
| `map_to_focused_output` | Follow the output holding keyboard focus. |
| `map_to_focused_window` | Map the tablet area to the focused window. |
| `left_handed` | Flip the tablet orientation. |
| `calibration_matrix` | Pass a six-number calibration matrix to libinput. |

Focused-window mapping takes precedence over focused-output mapping, which
takes precedence over `map_to_output`. When the selected target is unavailable,
the next configured mapping is used.

### Cursor

```toml
[input.cursor]
theme = ""
size = 24
hardware_cursor = true
follows_focus = false
hide_when_typing = false
hide_timeout_ms = 0
```

| Key | Range or values | Description |
| --- | --- | --- |
| `theme` | string | Xcursor theme; empty uses the environment default. |
| `size` | 1 to 512 | Cursor size in logical pixels. |
| `hardware_cursor` | bool | Use a hardware cursor plane when available. |
| `follows_focus` | bool | Follow keyboard-driven window focus. |
| `hide_when_typing` | bool | Hide after a non-modifier key press. |
| `hide_timeout_ms` | 0 to 3600000 | Hide after inactivity; `0` disables the timeout. |

Set `hardware_cursor = false` to work around hardware cursor flicker or
disappearance. Set `hide_timeout_ms` to a value from 1 to 3600000 to hide an
inactive cursor; `0` disables the timeout.

`follows_focus = true` moves the cursor to a newly focused window after
keyboard-driven focus and transfer actions. Pointer-driven focus, gestures, and
automatic replacement focus do not move it. `window-focus-warp:<id>` always
moves the cursor regardless of this setting.

### Focus

```toml
[input.focus]
follows_mouse = false
follows_mouse_max_scroll = 0.5
```

`follows_mouse = true` focuses the window under the pointer when pointer motion
or a layout change places a different window there. This follows the actual
seat focus across tiled, floating, and pinned windows, including pinned windows
whose owning workspace is inactive.

`follows_mouse_max_scroll` limits how far Umbriel may scroll a layout to reveal
that window, measured in viewport widths. `0.0` allows only fully visible
windows. Omit the key for no limit.
