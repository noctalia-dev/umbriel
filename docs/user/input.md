# Input devices

Configure keyboards, touchpads, mice, tablets, and touchscreens. Cursor, focus, and mouse window handling are in [Pointer and focus](pointer-and-focus.md); shortcuts are in [Keybinds](keybinds.md).

```toml
# A US and German layout, switched with Alt+Shift
[input.keyboard]
layout = "us,de"
options = "grp:alt_shift_toggle"
```

## Keyboard

```toml
[input.keyboard]
layout = ""
variant = ""
options = ""
model = ""
repeat_rate = 25
repeat_delay = 600
numlock_toggle = true
track_layout = "global"
```

| Key              | Default    | Description                                                                                         |
| ---------------- | ---------- | --------------------------------------------------------------------------------------------------- |
| `layout`         | `""`       | XKB layouts, comma-separated for several. Empty uses the system keymap.                             |
| `variant`        | `""`       | XKB variants, one per layout.                                                                       |
| `options`        | `""`       | XKB options, such as `caps:escape` or `compose:ralt`.                                               |
| `model`          | `""`       | XKB keyboard model.                                                                                 |
| `repeat_rate`    | `25`       | Key repeats per second, 0 to 1000; `0` disables repeat.                                             |
| `repeat_delay`   | `600`      | Milliseconds before a held key repeats, 0 to 10000.                                                 |
| `numlock_toggle` | `true`     | Turn NumLock on for the first keyboard.                                                             |
| `track_layout`   | `"global"` | `"global"` for one layout for the session, or `"window"` to remember the layout per focused window. |

When none of `layout`, `variant`, `options`, and `model` is set:

- Umbriel uses the keymap set with `localectl set-x11-keymap`, read at startup and on reload. Run `umbriel msg config-reload` after changing it.
- Without systemd-localed, the `XKB_DEFAULT_*` environment variables apply, then `us`.

Invalid values are logged and fall back to the system default. `xkbcli list` shows the available values.

In `"window"` tracking, a window that has never had focus starts on the first layout, and a reload clears the remembered layouts.

> [!NOTE]
> Lock keys are shared by every keyboard and survive reloads and hotplug. `numlock_toggle` only sets the initial NumLock state.

### Several layouts

Bind `keyboard-layout-next` to cycle the configured layouts; scripts can run `umbriel msg keyboard-layout-next`.

```toml
[keybinds]
"Mod+Shift+K" = "keyboard-layout-next"
```

`umbriel keyboard-layouts` lists the layouts and marks the active one with `*`. Virtual keyboards from applications keep their own keymap.

## Touchpad

```toml
[input.touchpad]
tap = true
natural_scroll = true
left_handed = false
```

Omitted keys keep the device's default.

| Key                         | Default        | Description                                                                                                        |
| --------------------------- | -------------- | ------------------------------------------------------------------------------------------------------------------ |
| `enabled`                   | `true`         | Enable touchpad input.                                                                                             |
| `tap`                       | device default | Tap to click.                                                                                                      |
| `tap_and_drag`              | device default | Keep dragging after a tap when the finger stays down and moves.                                                    |
| `drag_lock`                 | device default | After a tap-and-drag: `"off"`, `"timeout"` to end shortly after lifting, or `"sticky"` to hold until the next tap. |
| `natural_scroll`            | device default | Reverse scrolling and three-finger gesture direction.                                                              |
| `left_handed`               | device default | Swap the primary and secondary buttons.                                                                            |
| `accel_profile`             | device default | `"flat"`, `"adaptive"`, or a custom curve, as for the [mouse](#mouse).                                             |
| `sensitivity`               | device default | Pointer speed, -1.0 to 1.0.                                                                                        |
| `scroll_factor`             | unset          | Application scroll multiplier, 0.1 to 10.0, or per axis: `{ horizontal = 2.0, vertical = 1.5 }`.                   |
| `disable_while_typing`      | device default | Ignore the touchpad during keyboard input.                                                                         |
| `disable_on_external_mouse` | device default | Ignore the touchpad while an external mouse is connected.                                                          |
| `scroll_method`             | device default | `"two_finger"`, `"edge"`, or `"none"`.                                                                             |
| `click_method`              | device default | `"button_areas"` or `"clickfinger"`.                                                                               |
| `tap_button_map`            | device default | Buttons for one-, two-, and three-finger taps: `"left_right_middle"` or `"left_middle_right"`.                     |

The [`touchpad_scroll_factor`](window-rules-behavior.md#touchpad-scroll-speed) window rule scales `scroll_factor` further for matching windows. Overview scrolling has its own factors in [Workspaces Overview](workspaces-overview.md#settings-and-behavior).

> [!NOTE]
> A device supports only some scroll methods (`libinput list-devices` shows them); an unsupported one is logged and ignored. In a [per-device rule](#per-device-overrides), `scroll_method` and the mouse `scroll_button` replace each other.

## Mouse

```toml
[input.mouse]
natural_scroll = false
left_handed = false
sensitivity = 0.0
scroll_wheel_step = 60
```

| Key                  | Default        | Description                                                                                                                     |
| -------------------- | -------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `natural_scroll`     | device default | Reverse wheel direction.                                                                                                        |
| `left_handed`        | device default | Swap the primary and secondary buttons.                                                                                         |
| `accel_profile`      | device default | `"flat"`, `"adaptive"`, or a custom libinput curve such as `"custom 0.2 0.0 0.5 1.0 2.0"`.                                      |
| `sensitivity`        | `0.0`          | Pointer speed, -1.0 to 1.0.                                                                                                     |
| `scroll_wheel_step`  | `60`           | Logical pixels per layout scroll action, 1 to 1000. Scrolling inside applications is unaffected.                                |
| `scroll_button`      | unset          | Scroll by moving the mouse while this button is held: `MouseLeft`, `MouseRight`, `MouseMiddle`, `MouseBack`, or `MouseForward`. |
| `scroll_button_lock` | `false`        | Toggle `scroll_button` scrolling with a press instead of holding.                                                               |

## Tablet

```toml
[input.tablet]
enabled = true
map_to_output = "DP-1"
map_to_focused_output = false
map_to_focused_window = false
left_handed = false
calibration_matrix = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0]
```

| Key                     | Description                                        |
| ----------------------- | -------------------------------------------------- |
| `enabled`               | Enable tablet and pad input.                       |
| `map_to_output`         | Confine the tablet to a connector or monitor name. |
| `map_to_focused_output` | Follow the output holding keyboard focus.          |
| `map_to_focused_window` | Map the tablet area to the focused window.         |
| `left_handed`           | Flip the tablet orientation.                       |
| `calibration_matrix`    | Pass a six-number calibration matrix to libinput.  |

Focused-window mapping takes precedence over focused-output mapping, which takes precedence over `map_to_output`.

## Touch

```toml
[input.touch]
enabled = true
map_to_output = "eDP-1"
```

| Key             | Description                                         |
| --------------- | --------------------------------------------------- |
| `enabled`       | Enable or disable touch input.                      |
| `map_to_output` | Confine touch input to a connector or monitor name. |

Without `map_to_output`, a touchscreen maps to the output its device reports, or to the built-in panel (`eDP-`, `LVDS-`, or `DSI-`) when exactly one is enabled. Otherwise it spans all outputs.

A touchscreen reports its output through the `WL_OUTPUT` udev property. A udev rule can set it, so each touchscreen keeps its own monitor:

```text
SUBSYSTEM=="input", KERNEL=="event*", ATTRS{idVendor}=="1234", ATTRS{idProduct}=="5678", ENV{WL_OUTPUT}="DP-1"
```

## Per-device overrides

Use `[[input.device]]` for one device. `name` must exactly match the `Device` value from `libinput list-devices`, case included.

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

[[input.device]]
name = "Acme Controller Touchpad"
enabled = false
```

- Each rule starts from its keyboard, touchpad, or mouse settings and replaces only the keys it sets.
- `enabled = false` turns a device off.
- A keyboard rule that sets any of `layout`, `variant`, `options`, or `model` uses only its own and `[input.keyboard]`'s values, not the system keymap.
- Two rules for the same name are rejected.

## Clipboard

```toml
[input]
middle_click_paste = false
```

This disables primary-selection paste, including Shift+Insert. Ctrl+C and Ctrl+V are unaffected. Applications started while it is off need a restart after turning it back on.

## Input capture and controllers

Sharing your keyboard and mouse with another machine through the InputCapture portal is described in [Input capture](security.md#input-capture). Steam's desktop controller mappings ask for permission first; see [X11 desktop input](security.md#x11-desktop-input).
