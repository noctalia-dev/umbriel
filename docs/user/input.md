# Input

Configure keyboard, pointer, touchpad, tablet, cursor, and focus behavior under
`[input]`.

## Settings

```toml
[input]
middle_click_paste = false
client_window_drag = true
window_snapping = false
window_drag_toggle = "none"
mod_right_double_click = "none"
```

`middle_click_paste = false` disables primary-selection paste, including
Shift+Insert. The regular Ctrl+C and Ctrl+V clipboard is unaffected.
Applications started while primary selection is disabled must be restarted
after it is re-enabled.

`window_snapping = true` enables edge and corner drop zones for floating windows
in any layout, using title bar, client move, or Mod+drag. A preview shows the
placement before release. Left, right, top, and bottom edges snap to the
corresponding half of the screen. All four corners snap to quarters. Maximize
remains available through the title bar button, double-click, or an action.
The targets respect panels, layout struts, gaps, borders, title bars, and client
size limits, all in logical coordinates.

Dragging a snapped or maximized window away restores its normal size under the
pointer, keeping the title bar and content aligned while the application redraws.
Snapping on another monitor maps the normal restore position to that monitor.
Shared monitor edges require a 150 ms dwell before offering a zone, so
dragging straight across monitors remains possible. Tiled, pinned, scratchpad,
overview, data-device, and toplevel-transfer drags do not use these zones.
Keyboard actions `window-snap:<region>` and `window-unsnap` work independently
of the pointer setting. See [Actions](actions.md).

`client_window_drag = false` ignores move requests from applications, such as
dragging a client-side title bar or the empty tab strip of Chromium, Firefox,
or Electron apps. Ordinary window moves then use Mod+drag, keybinds, or an
enabled [Umbriel title bar](appearance.md#title-bar).
Resizing from client-side borders and tab tear-off operations using
`xdg-toplevel-drag-v1` are unaffected.

When the title bar is shown by `[appearance.title_bar] mode` or a window rule, dragging empty server-side title
bar space moves a window without a modifier. Double-click it to maximize or
restore. Floating windows also resize by dragging the frame's edges or corners
without a modifier. These compositor-owned interactions work with
`client_window_drag = false`.

Mod+right-click drags a window's resize edge. `mod_right_double_click` gives a
second Mod+right-click on the same window a job beyond resizing:

| Value | Behavior |
| --- | --- |
| `"none"` | (default) Mod+right-click only drags a resize edge. |
| `"maximize"` | A second Mod+right-click on the same window within 300 ms and 8 px of the first toggles maximize, with the same target a client's titlebar double-click picks: `client_maximize` decides between the column and the edges for a tiled window, and a floating window fills the usable area. |

With `"maximize"`, the first click starts resizing only once the pointer moves
more than 8 px from the press, and then resizes the edge under the press.
A `Mod+MouseRight` keybind overrides the built-in button entirely.

`window_drag_toggle` controls what pressing the other main mouse button does
during a window drag:

| Value | Behavior |
| --- | --- |
| `"none"` | Leave the drag unchanged. |
| `"floating"` | Toggle whether the dropped window is tiled or floating. |
| `"pinned"` | Toggle whether the dropped window is pinned. |

The state changes when the window is dropped. Unsupported transitions, such as
pinning a fullscreen window, leave the window unchanged.

### Detached tabs and panels

Umbriel supports `xdg-toplevel-drag-v1` for native Wayland applications that
implement it, including Chromium-based browsers. During a pointer drag, a
detached window follows the pointer above ordinary windows and stays outside
the tiling layout. Windows underneath it can receive the drop, allowing the
application to merge a tab or dock a panel. If the application keeps the
detached window after the drop, Umbriel places it using normal tiled or
floating window-drag behavior. No additional configuration is needed.

### Keyboard

```toml
[input.keyboard]
layout = ""           # empty uses the system keymap
variant = ""
options = ""
model = ""
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
layouts, variants, and models are reported in the log and fall back to the
system default. Run `xkbcli list` to inspect available values.

When `[input.keyboard]` sets none of `layout`, `variant`, `options`, and
`model`, Umbriel uses the X11 keymap configured with
`localectl set-x11-keymap`, read from systemd-localed at startup and on every
config reload. Run `umbriel msg config-reload` to apply a keymap changed with
`localectl`. Without systemd-localed, the `XKB_DEFAULT_*` environment
variables apply, then `us`. A `[[input.device]]` rule that sets any of these
keys uses only its own and `[input.keyboard]`'s values.

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

When an input method removes its virtual keyboard, Umbriel restores the keyboard
that supplied the input. Held shortcut modifiers stay active until they are
released, even when another connected keyboard is idle.

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
# tap_and_drag = false
# drag_lock = "sticky"
natural_scroll = true
left_handed = false
# accel_profile = "adaptive"
# sensitivity = 0.5
# scroll_factor = 1.5
# disable_while_typing = true
# disable_on_external_mouse = true
# scroll_method = "edge"
# click_method = "clickfinger"
# tap_button_map = "left_middle_right"
```

Omitted values preserve the device's libinput defaults. Explicit unsupported
settings are reported in the log.

| Key | Description |
| --- | --- |
| `tap` | Enable tap-to-click. |
| `tap_and_drag` | Keep dragging after a tap when the finger stays down and moves. |
| `drag_lock` | Keep dragging after the finger lifts during a tap-and-drag: `"off"` disables it, `"timeout"` ends the drag shortly after the finger lifts, `"sticky"` holds the drag until the next tap. |
| `natural_scroll` | Reverse scrolling and three-finger gesture direction. |
| `left_handed` | Swap the primary and secondary buttons. |
| `accel_profile` | Use `"flat"`, `"adaptive"`, or a custom acceleration curve. |
| `sensitivity` | Pointer speed from -1.0 to 1.0. |
| `scroll_factor` | Application scroll multiplier from 0.1 to 10.0. |
| `disable_while_typing` | Disable the touchpad during keyboard input. |
| `disable_on_external_mouse` | Disable the touchpad while an external mouse is connected. |
| `scroll_method` | Scroll gesture for application content: `"two_finger"`, `"edge"`, or `"none"` to disable. |
| `click_method` | Use `"button_areas"` or `"clickfinger"`. |
| `tap_button_map` | Buttons for one-, two-, and three-finger taps: `"left_right_middle"` or `"left_middle_right"`. |

`scroll_method` selects the gesture a touchpad-style device uses to scroll
applications. Devices support a subset (`libinput list-devices` reports them);
an unsupported value is logged and ignored. An `input.device` rule that sets
`scroll_method` replaces that device's `scroll_button` scrolling, including its
lock, and a rule that sets `scroll_button` takes the device's scrolling away
from the `[input.touchpad]` gesture. Mice and other device classes never
inherit `[input.touchpad] scroll_method`.

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
left_handed = false
# accel_profile = "flat"
sensitivity = 0.0
scroll_wheel_step = 60
# scroll_button = "MouseBack"
# scroll_button_lock = false
```

`sensitivity` ranges from -1.0 to 1.0. `scroll_wheel_step` accepts 1 to 1000
logical pixels per layout-scroll action. `left_handed` swaps the primary and
secondary buttons; omit it to preserve the device default.

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
model = "pc104angle"
repeat_rate = 40
repeat_delay = 250

[[input.device]]
name = "Acme Precision Touchpad"
tap = true
natural_scroll = false
left_handed = false
scroll_method = "edge"
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
takes precedence over `map_to_output`.

### Touch

```toml
[input.touch]
enabled = true
map_to_output = "eDP-1"
```

| Key | Description |
| --- | --- |
| `enabled` | Enable or disable touch input. |
| `map_to_output` | Confine touch input to a connector or monitor name. |

Without `map_to_output`, a touchscreen maps to the output its device reports,
or else to the built-in panel (`eDP-`, `LVDS-`, or `DSI-`) when exactly one is
enabled. Otherwise it spans the whole output layout.

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
moves the cursor regardless of this setting. Active-workspace output swaps keep
the cursor and seat focus on the invoking output when this setting is false;
when it is true, both follow the previously focused window to its new output.

### Drag edge scroll

```toml
[input.drag_edge_scroll]
enabled = true
trigger_zone = 30
delay_ms = 100
max_speed = 1500.0
```

While a drag-and-drop or a tiled window move (Mod+left-click or the tab bar)
is held near either end of a scrolling layout, the layout scrolls so offscreen
columns can be reached.

| Key | Range or values | Description |
| --- | --- | --- |
| `enabled` | bool | Enable edge scrolling during a data drag or a tiled window move. |
| `trigger_zone` | 1 to 1000 | Width of the zone at each end, in logical pixels. |
| `delay_ms` | 0 to 10000 | Time inside the zone before scrolling starts. |
| `max_speed` | 1.0 to 100000.0 | Peak scroll speed in logical pixels per second. |

Speed rises from zero at the inner edge of the trigger zone to `max_speed` at
the outer edge of the layout, so scrolling accelerates as the pointer pushes
further in. The zone is clamped to half the viewport, so the two zones never
overlap. `delay_ms = 0` starts scrolling as soon as the pointer enters the
zone. `enabled = false` turns the behavior off entirely.

### Focus

```toml
[input.focus]
follows_mouse = false
follows_mouse_max_scroll = 0.5
```

`follows_mouse = true` focuses the window that the pointer enters through real
pointer motion. Scrolling or dragging a layout beneath a stationary pointer
does not change focus. This follows the actual seat focus across tiled,
floating, and pinned windows, including pinned windows whose owning workspace
is inactive.

`follows_mouse_max_scroll` limits how far Umbriel may scroll a layout to reveal
that window, measured in viewport widths. `0.0` allows only fully visible
windows. Omit the key for no limit.
