# Keybinds

Configure bindings under `[keybinds]`. See [Actions](actions.md) for the
complete action list.

```toml
[keybinds]
"Mod+T" = "spawn:kitty"
"Mod+Q" = "window-close"
"Mod+Left" = "window-focus-left"
"Mod+Right" = "window-focus-right"
"Mod+I" = "overview-toggle"
```

## Modifiers

| Modifier | Notes |
| --- | --- |
| `Mod` | Uses `general.mod_key`; defaults to Super on DRM and Alt when nested. |
| `Shift` | |
| `Ctrl` or `Control` | |
| `Alt` | |
| `Super`, `Logo`, or `Win` | |

Bare keys such as `XF86AudioMute` are also valid.

A modifier can be bound by itself:

```toml
"Mod" = "spawn:noctalia msg panel-toggle launcher"
```

Modifier-only binds run on release when no other key, button, scroll, touch, or
gesture input occurred while the modifier was held. Pointer motion alone does
not cancel them.

## Special keys

- Wheel: `WheelUp`, `WheelDown`, `WheelLeft`, `WheelRight`
- Mouse: `MouseLeft`, `MouseRight`, `MouseMiddle`, `MouseBack`, `MouseForward`

Mouse and wheel binds require at least one modifier:

```toml
"Mod+MouseMiddle" = "layout-scroll-drag"
```

## Consumed input

A matched bind consumes its press and release, so neither reaches the focused
application. Unbound input is delivered normally.

## Repeat

Binds repeat using the configured keyboard rate and delay, including `spawn:`
binds, so held volume and brightness keys keep stepping. Disable repeat for one
bind, such as a launcher, with the table form:

```toml
"Mod+Return" = { action = "spawn:kitty", repeat = false }
```

Scratchpad visibility and cycling actions never repeat. The built-in `Mod+Q`
and `Mod+O` binds do not repeat either.

## Allow when locked

Binds are blocked while the session is locked unless explicitly allowed:

```toml
"XF86MonBrightnessDown" = { action = "spawn:noctalia msg brightness-down 10", allow_when_locked = true }
```

Use this only for actions that are safe without an unlocked session.

## Keyboard shortcuts inhibition

Games and remote desktop clients can ask Umbriel to pass keyboard shortcuts
through. Pointer and wheel binds are unaffected.

Keep one escape binding available:

```toml
"Mod+Shift+Escape" = { action = "shortcuts-inhibit-toggle", allow_when_inhibited = true, repeat = false }
```

## Cooldown

`cooldown_ms` suppresses repeated actions for a period while continuing to
consume matching input:

```toml
"Mod+WheelUp" = { action = "workspace-previous", cooldown_ms = 150 }
"Mod+WheelDown" = { action = "workspace-next", cooldown_ms = 150 }
```

## Submaps

Submaps are temporary keybind layers. Enter one with `submap:<name>` and leave
one level with `submap:reset`.

Prefix bindings inside a submap with `submap[name],`:

```toml
"Mod+S" = { action = "submap:screencapture", repeat = false }
"submap[screencapture],1" = { action = "spawn:grim screenshot.png", submap = "reset" }
"submap[screencapture],2" = { action = "submap:region", repeat = false }
"submap[screencapture],Escape" = "submap:reset"
"submap[region],R" = { action = "spawn:grim -g \"$(slurp)\" screenshot.png", submap = "reset" }
"submap[region],Escape" = "submap:reset"
```

The optional `submap` field applies a transition after the action. Use
`submap = "reset"` for one-shot commands. A default-context
`"Escape" = "submap:reset"` always matches, even outside a submap, so use it
only when bare Escape should be consumed globally.

Run `umbriel submap` to print the active layer, or subscribe to `submap` events
for a panel or script.

## Keyboard layouts

Bindings normally match the symbol produced by the active layout. When several
layouts are configured, Umbriel also checks the same physical key for a
printable ASCII symbol in the other layouts. This keeps binds such as `Mod+T`
working after switching to a non-Latin layout.

A keyboard configured with only a non-Latin layout has no ASCII fallback. Add
an alternate layout or bind the active XKB keysym name.

## Hot corners

Hot corners run an action after the pointer rests in an output corner:

```toml
[hot_corners.top_left]
enabled = true
delay_ms = 500
action = "overview-open"
```

Available sections are `top_left`, `top_right`, `bottom_left`, and
`bottom_right`.

| Key | Default | Description |
| --- | --- | --- |
| `enabled` | `false` | Enable this corner. |
| `delay_ms` | `500` | Delay from 0 to 10000 milliseconds. |
| `action` | unset | Any action accepted by a keybind. |

Hot corners are inactive while the output's focused window is fullscreen.

## Cheatsheet

The cheatsheet lists active keybinds. It opens at startup when
`general.show_cheatsheet` is enabled. Use `cheatsheet-toggle`,
`cheatsheet-open`, or `cheatsheet-close` from a bind or `umbriel msg`.

Any non-modifier key or mouse button closes it. Normal bound actions still run.

## Example: Noctalia shell integration

```toml
"Mod" = "spawn:noctalia msg panel-toggle launcher"
"Mod+V" = "spawn:noctalia msg panel-toggle clipboard"
"Mod+W" = "spawn:noctalia msg panel-toggle wallpaper"
"Mod+P" = "spawn:noctalia msg screenshot-region"
"Mod+Escape" = "spawn:noctalia msg panel-toggle session"
```

## Example: direct primary extents

```toml
"Mod+A" = "window-set-primary-extent:0.333"
"Mod+S" = "window-set-primary-extent:0.5"
"Mod+D" = "window-set-primary-extent:0.667"
"Mod+F" = "window-set-primary-extent:1.0"
"Mod+R" = "window-cycle-primary-extent"
"Mod+Shift+R" = "window-cycle-primary-extent-back"
```

## Example: scroll-wheel navigation

```toml
"Mod+WheelUp" = "window-focus-left"
"Mod+WheelDown" = "window-focus-right"
"Mod+Shift+WheelUp" = "column-move-left"
"Mod+Shift+WheelDown" = "column-move-right"
```

## Example: media and brightness keys

```toml
"XF86AudioRaiseVolume" = "spawn:wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+"
"XF86AudioLowerVolume" = "spawn:wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"
"XF86AudioPlay" = "spawn:playerctl play-pause"
"XF86AudioNext" = "spawn:playerctl next"
"XF86AudioPrev" = "spawn:playerctl previous"
"XF86MonBrightnessUp" = "spawn:brightnessctl set +5%"
"XF86MonBrightnessDown" = "spawn:brightnessctl set 5%-"
```

Volume commands require `wpctl`, media commands require `playerctl`, and
brightness commands require `brightnessctl`.
