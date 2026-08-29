# Keybinds

All keybinds live under `[keybinds]`. Chords are case-insensitive.

```toml
[keybinds]
"Mod+T" = "spawn:kitty"
"Mod+Shift+Q" = "window-close"
"Mod+I" = "overview-toggle"
```

## Keyboard layouts

Keybinds continue to match the effective symbol from the active keyboard
layout. If the key's unmodified (level 0) symbol is printable non-ASCII,
Umbriel also checks the same physical key in the keyboard's other configured
layouts, in order, for a printable ASCII fallback. This keeps bindings such as
`Mod+T` working after switching from `us` to a non-Latin layout while retaining
active-layout symbol matching.

Umbriel does not load an implicit reference layout. A keyboard configured with
only a non-Latin layout, or whose other layouts have no printable ASCII symbol
on that key, has no ASCII fallback. Add a suitable alternate to that keyboard's
layout list (see [Input](input.md)), or bind the active layout's XKB keysym
name.

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

The complete action reference is in [Actions](actions.md). This page covers
how bindings are written and how they behave.

## Repeat

Binds repeat while held, using `input.keyboard.repeat_rate` and
`repeat_delay`. Opt out per bind with the table form:

```toml
"Mod+Return" = { action = "spawn:kitty", repeat = false }
```

Scratchpad visibility and cycling actions never repeat, even if their binding
does not set `repeat = false`.

Binds with `allow_when_locked = true` continue repeating while the session is locked.

## Allow when locked

Binds are blocked by default when the session is locked. Opt in per bind
with the table form:

```toml
"XF86MonBrightnessDown" = { action = "spawn:noctalia msg brightness-down 10", allow_when_locked = true }
```

## Submaps

Submaps are temporary keybind layers that can be nested. Enter a named layer
with `submap:<name>` and exit one level with `submap:reset`. Set entry binds to
`repeat = false` so holding the key cannot push the same layer more than once.

A table-form bind can optionally set `submap = "reset"` to exit one level after
its action, or `submap = "<name>"` to enter a nested layer. The action runs
before the transition. Omitting `submap` adds no post-action transition, so the
action alone determines the resulting layer. Binds with a post-action
transition never repeat.

Binds inside a submap prefix the chord with `submap[name],`:

```toml
"Mod+S" = { action = "submap:screencapture", repeat = false }
"submap[screencapture],1" = { action = "spawn:grim screenshot.png", submap = "reset" }
"submap[screencapture],2" = { action = "submap:region", repeat = false }
"submap[screencapture],Escape" = "submap:reset"
"submap[region],R" = { action = "spawn:grim -g \"$(slurp)\" screenshot.png", submap = "reset" }
"submap[region],Escape" = "submap:reset"
```

Run `umbriel submap` to print the active submap name. It emits no text when the
stack is empty. The JSON form returns `null` for that state:

```console
$ umbriel submap
$ umbriel submap --json
null
$ umbriel msg submap:screencapture
$ umbriel submap
screencapture
$ umbriel submap --json
"screencapture"
$ umbriel msg submap:reset
$ umbriel submap
```

An empty name cannot be created through configuration or `umbriel msg`. If an
internal caller supplies one, human output uses `unnamed` and JSON preserves the
empty string.

For nested submaps, the command reports the top layer. One `submap:reset`
reveals the previous layer; resetting the last layer makes the human command
silent again.

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
# Volume
"XF86AudioRaiseVolume" = "spawn:wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+"
"XF86AudioLowerVolume" = "spawn:wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"
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

Volume control requires `wpctl` (from WirePlumber/PipeWire) while media playback requires `playerctl`.
