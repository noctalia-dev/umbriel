# Workspace Overview

This page covers the workspace overview and hot corners.

## Settings and behavior

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
or use one of the [overview actions](actions.md#overview-actions).

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
their windows. They also retain each surface's color description, so HDR and
extended-linear content keeps the same appearance while the overview is open.
`workspace_background` adds a rounded background behind each workspace. Its
alpha can produce anything from a light tint to an opaque fill.

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
