# Keybinds

All keybinds live under `[keybinds]`. Chords are case-insensitive.

```toml
[keybinds]
"Mod+T" = "spawn:kitty"
"Mod+Shift+Q" = "window-close"
"Mod+I" = "overview-toggle"
```

## Modifiers

| Modifier | Notes |
|----------|-------|
| `Mod` | Configured by `general.mod_key`; defaults to Alt when nested and Super on DRM. |
| `Shift` | |
| `Ctrl` / `Control` | |
| `Alt` | |
| `Super` / `Logo` / `Win` | |

Bare keys are also allowed (e.g. `XF86AudioMute`).

A modifier can also be bound by itself:

```toml
"Mod" = "spawn:noctalia msg panel-toggle launcher"
```

Modifier-only binds run on release when no other discrete input occurred while
the modifier was held. Any other key press, mouse button, scroll, touch down, or
gesture cancels the action. Pointer motion alone does not cancel it. Both the
left and right key for the logical modifier are accepted, and modifier-only
binds never repeat. Combinations containing only multiple modifiers, such as
`Ctrl+Alt`, are invalid.

## Special keys

**Scroll wheel:** `WheelUp`, `WheelDown`, `WheelLeft`, `WheelRight` (require
at least one modifier).

**Mouse buttons:** `MouseLeft`, `MouseRight`, `MouseMiddle`, `MouseBack`,
`MouseForward` (require at least one modifier).

**Defaults:** `Mod+WheelUp` = `window-focus-left`, `Mod+WheelDown` =
`window-focus-right`.

Mouse and wheel chords combine the modifier state of every keyboard, as
keyboard chords do. They remain active while an input method grabs a physical
keyboard and injects composed text through its own virtual keyboard.

During an active tiled `Mod+MouseLeft` drag, `window-focus-left` and
`window-focus-right` wheel binds scroll the strip instead of trying to move
focus away from the detached window. Wheel-driven strip scrolling uses twice
the configured step while dragging. The insertion hint and drop target follow
the newly exposed columns without requiring additional pointer motion.

## Actions

Run `umbriel msg --help` for the full list. Default keybinds are only loaded when
no config file exists; once you provide a config, `[keybinds]` is the complete
set.

### Parameterized actions

| Action | Parameter | Example |
|--------|-----------|---------|
| `spawn:<cmd>` | Shell command | `"spawn:kitty"` |
| `workspace-switch:<ws>` | Workspace name, optionally `/<output>` | `"workspace-switch:3"`, `"workspace-switch:CHAT/HDMI-A-1"` |
| `window-move-to-workspace:<ws>` | Same as above | `"window-move-to-workspace:2"` |
| `window-set-width:<frac>` | Fraction 0.1-1.0 | `"window-set-width:0.667"` |
| `window-modify-width:<delta>` | Signed fraction -0.9..0.9; the resulting width clamps to 0.1..1.0 | `"window-modify-width:-0.2"` |
| `workspace-set-layout:<scrolling\|dwindle\|toggle>` | Switch the active workspace's layout at runtime; sticky until a config reload reasserts the configured mode | `"workspace-set-layout:toggle"` |
| `window-focus:<window-id>` | Window id from `umbriel windows` | `"window-focus:0123abcd"` |
| `window-close[:<window-id>]` | Optional window id; bare form closes the focused window | `"window-close"` |
| `dpms-off[:<output>]` / `dpms-on[:<output>]` | Optional connector name; bare form targets every configured output | `"dpms-off:DP-1"`, `"dpms-on"` |
| `session-quit[:skip-confirmation]` | Bare form opens an on-screen confirmation (Enter or the quit bind confirms; any other key or click cancels); `skip-confirmation` quits immediately | `"session-quit:skip-confirmation"` |

A second `session-quit` while the confirmation is open also quits. While the
session is locked, `session-quit` quits without the dialog.

Workspace selectors use exact names. Numeric selectors prefer the focused
output: an exact numeric name wins, otherwise the number selects that 1-based
position in a static custom-named workspace list. On a dynamic output, a number
beyond the current workspace list selects the last workspace. Unique names
resolve globally; duplicate names resolve on the preferred output. Add
`/output` to target another output explicitly.

When `workspace-switch` targets a workspace on another monitor, the cursor warps
to the center of that monitor, so focus follows the switch.

### Window and layout actions

These take no argument.

| Action | What it does |
|--------|--------------|
| `window-focus-left` / `window-focus-right` | Move focus to the adjacent window along the row. |
| `window-focus-up` / `window-focus-down` | Move focus to the adjacent window along the column. |
| `window-focus-next` | Cycle focus to the next mapped window on the active workspace. |
| `window-move-to-workspace-next` / `window-move-to-workspace-previous` | Move the focused window to the adjacent workspace and follow it. These actions do not wrap around. |
| `column-move-left` / `column-move-right` | Move the focused window's column left or right. |
| `window-move-up` / `window-move-down` | Move the focused window up or down within its column. |
| `window-consume-left` | Pull the focused window into the column to its left. |
| `window-expel-right` | Pop the focused window out of its column into a new column to the right. |
| `window-cycle-width` | Cycle the focused column through its preset widths. |
| `window-cycle-width-back` | Cycle the focused column through its preset widths in reverse. |
| `window-toggle-fullscreen` | Toggle fullscreen for the focused window. |
| `window-toggle-maximize` | Toggle the focused column's full-width state. |
| `window-toggle-maximize-to-edges` | Toggle maximization of the focused window to the usable area's edges, without gaps or borders. Layer-shell exclusive zones remain visible. |
| `layout-scroll-left` / `layout-scroll-right` | Scroll the active workspace's scrolling-layout viewport; a no-op on a dwindle workspace. |
| `layout-scroll-up` / `layout-scroll-down` | Scroll toward strip start or end. These are first-class synonyms for `layout-scroll-left` and `layout-scroll-right`. |
| `config-reload` | Reload the config file, the same reload that runs automatically when the file changes on disk. |

On a vertical scrolling workspace, directional actions follow their visual
directions. `window-focus-left` and `window-focus-right` move within a lane;
`window-focus-up` and `window-focus-down` walk lanes. Likewise,
`column-move-left` and `column-move-right` reorder within a lane, while
`window-move-up` and `window-move-down` move the lane along the strip.
`layout-scroll-left` and `layout-scroll-up` both scroll toward strip start;
their right and down forms scroll toward strip end.

The default Mod+wheel bindings invoke `window-focus-left` and
`window-focus-right`, so they move within a lane on a vertical workspace.
Vertical-heavy configurations should bind wheel chords to
`window-focus-up` and `window-focus-down`, or to `layout-scroll-up` and
`layout-scroll-down`.

### Floating action

`window-toggle-floating` remembers the window's floating size and position.
The first time a window floats, Umbriel places it slightly below and to the
right of its tiled position while keeping it on-screen.

`window-toggle-pinned` makes the focused window float and keeps it above
fullscreen windows on its output. Pinned windows remain visible when you
switch workspaces. You cannot pin a fullscreen window, and making a pinned
window fullscreen removes its pinned state.

### Output and movement actions

`workspace-next` and `workspace-previous` switch to the adjacent workspace on the
focused output, by index. They do not wrap around: `workspace-previous` on the
first workspace is a silent no-op. On a dynamic output, `workspace-next` reaches
the trailing empty workspace, which becomes active as usual.

`workspace-move-down` and `workspace-move-up` move the focused workspace up or down
on the focused output. They do not wrap around either.

The matching window actions can be bound independently:

```toml
[keybinds]
"Mod+Shift+Comma" = "window-move-to-workspace-previous"
"Mod+Shift+Period" = "window-move-to-workspace-next"
```

`window-center` centers the focused floating window on its output's usable
area. It is a no-op while a tiled window is focused.

The directional output actions target the adjacent monitor:

| Action | What it does |
|--------|--------------|
| `output-focus-left` / `output-focus-right` / `output-focus-up` / `output-focus-down` | Move focus to the adjacent monitor in that direction. |
| `window-move-to-output-left` / `window-move-to-output-right` / `window-move-to-output-up` / `window-move-to-output-down` | Move the focused window to the adjacent monitor's active workspace. |
| `column-move-to-output-left` / `column-move-to-output-right` / `column-move-to-output-up` / `column-move-to-output-down` | Move the focused window's whole column to the adjacent monitor's active workspace. |
| `workspace-move-to-output-left` / `workspace-move-to-output-right` / `workspace-move-to-output-up` / `workspace-move-to-output-down` | Move every window of the active workspace to the adjacent monitor, preserving column order and widths. |

Directions do not wrap around: with no monitor in that direction the action
fails with an IPC error ("no output to the left" and friends). The cursor warps
to the center of the target monitor, so focus follows the action. Floating
windows keep their relative position on the new monitor; a column moved onto a
dwindle output flattens into single-window columns, the same as drag-and-drop.

### Overview actions

Use `overview-toggle`, `overview-open`, or `overview-close`.

Windows can be dragged onto another workspace preview. With dynamic numbered
workspaces, dropping a window into the gap between two previews, or into the
gap above the first preview, creates a new workspace at that position and
shifts the following workspace numbers down.
Umbriel keeps one empty dynamic workspace, so other previews disappear as soon
as their last window is moved or closed, including while the overview is open.
Static configured workspace lists only accept drops onto existing previews.

### Cheatsheet actions

Use `cheatsheet-toggle`, `cheatsheet-open`, or `cheatsheet-close`.

The cheatsheet lists every active keybind. It opens at startup when
`general.show_cheatsheet` is `true`, which is the default. You can also toggle
it through IPC with `umbriel msg cheatsheet-toggle`.

Any non-modifier key or mouse button closes the cheatsheet. Bound key
combinations still run normally. A click used to close the cheatsheet is not
passed to the window beneath it.

### Keyboard layout action

`keyboard-layout-next` activates the next layout in `input.keyboard.layout` and
wraps at the end, on every physical keyboard. It is inert when only one layout
is configured, and virtual keyboards keep the keymap their client supplied.

```toml
[input.keyboard]
layout = "us,de"

[keybinds]
"Mod+Shift+K" = "keyboard-layout-next"
```

`umbriel msg keyboard-layout-next` does the same from a script or panel. An XKB
toggle such as `options = "grp:alt_shift_toggle"` is an alternative that lives
in the keymap itself; the two can coexist.

### Scratchpad actions

Each output has a holding area for windows that should stay nearby without
remaining on a workspace.

| Action | What it does |
|--------|--------------|
| `window-move-to-scratchpad` | Move the focused window from its workspace into the scratchpad. |
| `scratchpad-toggle` | Show or hide the output's scratchpad windows. |
| `window-restore-from-scratchpad` | Return the focused scratchpad window to its saved workspace. |
| `scratchpad-focus-next` | Focus the next visible scratchpad window. |

Add `:<output>` to any action to target a specific output, for example
`scratchpad-toggle:DP-1`. Without a suffix, the action targets the output under
the pointer.

See [Scratchpads](scratchpad.md) for setup examples, the full workflow,
multi-output behavior, restoration rules, and troubleshooting.

## Repeat

Binds repeat while held, using `input.keyboard.repeat_rate` and
`repeat_delay`. Opt out per bind with the table form:

```toml
"Mod+Return" = { action = "spawn:kitty", repeat = false }
```

Scratchpad visibility and cycling actions never repeat, even if their binding
does not set `repeat = false`.

## Submaps

Submaps are temporary keybind layers that can be nested. Enter with
`submap:<name>`, exit one level with `submap:reset`.

Binds inside a submap prefix the chord with `submap[name],`:

```toml
"Mod+S" = "submap:screencapture"
"submap[screencapture],1" = "spawn:grim screenshot.png"
"submap[screencapture],2" = "submap:region"
"submap[screencapture],Escape" = "submap:reset"
"submap[region],R" = "spawn:grim -g 'slurp -p' screenshot.png"
"submap[region],Escape" = "submap:reset"
```

These bindings capture through `grim` and `slurp` over wlr-screencopy.
Applications that capture through xdg-desktop-portal (browser screen sharing,
OBS, portal-aware screenshot tools) are served by the Screencast and Screenshot
interfaces implemented by
[xdg-desktop-portal-umbriel](https://github.com/noctalia-dev/xdg-desktop-portal-umbriel).
Window sharing renders an isolated copy of the selected window and its own
popups. Desktop backgrounds, other windows, compositor opacity, blur, borders,
and shadows are never included in that window stream.

A `submap:reset` bound in the default context (no prefix) always matches, even
inside a submap, as a global emergency exit:

```toml
"Escape" = "submap:reset"
```

## Example: Noctalia shell integration

[Noctalia](https://github.com/noctalia-dev/noctalia) exposes panels, screenshots,
and widgets via `noctalia msg`. Typical bindings:

```toml
"Mod" = "spawn:noctalia msg panel-toggle launcher"
"Mod+Z" = "spawn:noctalia msg panel-toggle launcher /emo"
"Mod+V" = "spawn:noctalia msg panel-toggle clipboard"
"Mod+W" = "spawn:noctalia msg panel-toggle wallpaper"
"Mod+N" = "spawn:noctalia msg panel-toggle noctalia/notes:panel"
"Mod+X" = "spawn:noctalia msg bar-toggle"
"Mod+P" = "spawn:noctalia msg screenshot-region"
"Mod+Shift+P" = "spawn:noctalia msg screenshot-fullscreen"
"Mod+Shift+W" = "spawn:noctalia msg desktop-widgets-toggle-edit"
"Mod+Escape" = "spawn:noctalia msg panel-toggle session"
```

## Example: direct column widths

```toml
"Mod+A" = "window-set-width:0.333"
"Mod+S" = "window-set-width:0.5"
"Mod+D" = "window-set-width:0.667"
"Mod+F" = "window-set-width:1.0"
```

## Example: scroll-wheel navigation

```toml
"Mod+WheelUp" = "window-focus-left"
"Mod+WheelDown" = "window-focus-right"
"Mod+Shift+WheelUp" = "column-move-left"
"Mod+Shift+WheelDown" = "column-move-right"
"Mod+MouseMiddle" = "overview-toggle"
```

## Example: media and brightness keys

```toml
# Volume (via Noctalia OSD)
"XF86AudioRaiseVolume" = "spawn:noctalia msg volume-up 2%"
"XF86AudioLowerVolume" = "spawn:noctalia msg volume-down 2%"
"Mod+XF86AudioMute" = "spawn:wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"

# Media playback (playerctl)
"XF86AudioPlay" = "spawn:playerctl play-pause"
"XF86AudioNext" = "spawn:playerctl next"
"XF86AudioPrev" = "spawn:playerctl previous"

# Brightness
"XF86MonBrightnessUp" = "spawn:brightnessctl set +5%"
"XF86MonBrightnessDown" = "spawn:brightnessctl set 5%-"
```

XF86 keys accept the same `Mod`, `Ctrl`, `Alt`, `Shift`, and `Super`
combinations as other keys. Modifier chords also work when the XF86 key is
reported by a separate laptop hotkey device.
