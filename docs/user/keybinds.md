# Keybinds

Configure bindings under `[keybinds]`. See [Actions](actions.md) for the complete action list.

```toml
[keybinds]
"Mod+T" = "spawn:kitty"
"Mod+Q" = "window-close"
"Mod+Left" = "window-focus-left"
"Mod+Right" = "window-focus-right"
"Mod+I" = "overview-toggle"
```

## Default keybinds

The packaged starting configuration ([`examples/config.toml`](https://github.com/noctalia-dev/umbriel/blob/main/examples/config.toml)) binds these, among others. `Mod` is Super on a native session and Alt when Umbriel runs nested in another desktop (see [Modifiers](#modifiers)).

| Shortcut               | Action                                      |
| ---------------------- | ------------------------------------------- |
| Mod                    | Toggle the Noctalia launcher                |
| Mod+Return             | Open kitty                                  |
| Mod+Q                  | Close the focused window                    |
| Mod+Escape             | Quit (asks for confirmation)                |
| Mod+F1                 | Cycle window focus                          |
| Mod+H/J/K/L or arrows  | Focus adjacent window                       |
| Mod+Shift+arrows       | Move column left/right or window up/down    |
| Mod+Comma / Mod+Period | Consume left / consume right                |
| Mod+R / Mod+F          | Cycle width / toggle fullscreen             |
| Mod+T                  | Toggle floating for the focused window      |
| Mod+P                  | Toggle pin for the focused window           |
| Mod+W                  | Toggle tabs for the focused column          |
| Mod+O                  | Toggle the overview                         |
| Mod+1..9               | Switch workspace on focused monitor         |
| Mod+Shift+1..9         | Move focused window to workspace and follow |

Change the launcher and terminal commands under `[keybinds]` if you use another shell or terminal.

## Modifiers

| Modifier                  | Notes                                                                 |
| ------------------------- | --------------------------------------------------------------------- |
| `Mod`                     | Uses `general.mod_key`; defaults to Super on DRM and Alt when nested. |
| `Shift`                   |                                                                       |
| `Ctrl` or `Control`       |                                                                       |
| `Alt`                     |                                                                       |
| `Super`, `Logo`, or `Win` |                                                                       |

Bare keys such as `XF86AudioMute` are also valid.

A modifier can be bound by itself:

```toml
"Mod" = "spawn:noctalia msg panel-toggle launcher"
```

Modifier-only binds run on release when no other key, button, scroll, touch, or gesture input occurred while the modifier was held. Pointer motion alone does not cancel them.

## Special keys

- Wheel: `WheelUp`, `WheelDown`, `WheelLeft`, `WheelRight`
- Mouse: `MouseLeft`, `MouseRight`, `MouseMiddle`, `MouseBack`, `MouseForward`

Mouse and wheel binds require at least one modifier:

```toml
"Mod+MouseMiddle" = "layout-scroll-drag"
```

## Consumed input

A matched bind consumes its press and release, so neither reaches the focused application. Unbound input is delivered normally.

## Repeat

Binds repeat using the configured keyboard rate and delay, including `spawn:` binds, so held volume and brightness keys keep stepping. Disable repeat for one bind, such as a launcher, with the table form:

```toml
"Mod+Return" = { action = "spawn:kitty", repeat = false }
```

Scratchpad visibility and cycling actions never repeat. The built-in `Mod+Q` and `Mod+O` binds do not repeat either.

## Allow when locked

Binds are blocked while the session is locked unless explicitly allowed:

```toml
"XF86MonBrightnessDown" = { action = "spawn:noctalia msg brightness-down 10", allow_when_locked = true }
```

Use this only for actions that are safe without an unlocked session.

## Keyboard shortcuts inhibition

Games and remote desktop clients can ask Umbriel to pass keyboard shortcuts through. You can also enable passthrough manually with `shortcuts-inhibit-toggle`, even when the focused client has not requested it. Keys that would trigger ordinary bindings then reach the client. Pointer and wheel binds are unaffected.

Keep one escape binding available:

```toml
"Mod+Shift+Escape" = { action = "shortcuts-inhibit-toggle", allow_when_inhibited = true, repeat = false }
```

Press the binding again to restore ordinary shortcuts. Bindings marked `allow_when_inhibited = true` remain available in either mode.

Manual passthrough belongs to the exact keyboard-focused surface. It pauses when focus moves to another surface and resumes when that surface regains focus. Popups and other surfaces do not inherit it. Destroying the surface clears its manual state. The toggle requires an unlocked session with a focused surface; locking preserves the setting for when focus returns after unlocking.

Use a [window rule](window-rules.md#keyboard-shortcuts-inhibition) with `allow_shortcuts_inhibit = false` to deny automatic client requests for selected applications. The manual toggle still enables passthrough for those windows.

When the client already has an allowed shortcuts inhibitor, the toggle activates or deactivates that inhibitor. Rule changes preserve a manual deactivation. If the client creates one while manual passthrough is active, one toggle disables both sources together. Destroying a client inhibitor does not clear an existing manual setting.

## Cooldown

`cooldown_ms` suppresses repeated actions for a period while continuing to consume matching input:

```toml
"Mod+WheelUp" = { action = "workspace-previous", cooldown_ms = 150 }
"Mod+WheelDown" = { action = "workspace-next", cooldown_ms = 150 }
```

## Submaps

Submaps are temporary keybind layers. Enter one with `submap:<name>` and leave one level with `submap:reset`.

Prefix bindings inside a submap with `submap[name],`:

```toml
"Mod+S" = { action = "submap:screencapture", repeat = false }
"submap[screencapture],1" = { action = "spawn:grim screenshot.png", submap = "reset" }
"submap[screencapture],2" = { action = "submap:region", repeat = false }
"submap[screencapture],Escape" = "submap:reset"
"submap[region],R" = { action = "spawn:grim -g \"$(slurp)\" screenshot.png", submap = "reset" }
"submap[region],Escape" = "submap:reset"
```

The optional `submap` field applies a transition after the action. Use `submap = "reset"` for one-shot commands. A default-context `"Escape" = "submap:reset"` always matches, even outside a submap, so use it only when bare Escape should be consumed globally.

Run `umbriel submap` to print the active layer, or subscribe to `submap` events for a panel or script.

## Keyboard layouts

Bindings normally match the symbol produced by the active layout. When several layouts are configured, Umbriel also checks the same physical key for a printable ASCII symbol in the other layouts. This keeps binds such as `Mod+T` working after switching to a non-Latin layout.

A keyboard configured with only a non-Latin layout has no ASCII fallback. Add an alternate layout or bind the active XKB keysym name.

## Hot corners

Hot corners run an action after the pointer rests in an output corner:

```toml
[hot_corners.top_left]
enabled = true
delay_ms = 500
action = "overview-open"
```

Available sections are `top_left`, `top_right`, `bottom_left`, and `bottom_right`.

| Key        | Default | Description                         |
| ---------- | ------- | ----------------------------------- |
| `enabled`  | `false` | Enable this corner.                 |
| `delay_ms` | `500`   | Delay from 0 to 10000 milliseconds. |
| `action`   | unset   | Any action accepted by a keybind.   |

Named effect `set` and `cycle` arguments are checked against preset/pool kind when configuration loads, including submap bindings and disabled hot corners. Invalid references reject the binding or clear the corner action with a source diagnostic. Only the final retained action is checked after replacements. Keybinds and enabled corners prepare all reached pool members and border overlays before a trigger; disabled corners contribute no compilation roots. See [Effect actions](actions.md) and [Effects](effects.md#runtime-selection).

Hot corners are inactive while the output's focused window is fullscreen. This check follows the output containing the corner, including when keyboard focus remains on another monitor. A fullscreen window on a different output or an inactive workspace does not block the corner. Neither does a retained fullscreen window after another window on that output takes focus.

An exclusive-keyboard layer blocks hot corners while it is mapped: a modal launcher or lock-like surface owns input, and a corner must not fire behind it. The shell's own overview chrome is exempt — exclusive-keyboard layers that the overview coexists with (such as a type-to-launch keyboard capture) do not block corners while the overview is open; a closing overview is back to the normal rule.

## Cheatsheet

The cheatsheet lists active keybinds. It opens at startup when `general.show_cheatsheet` is enabled. Use `cheatsheet-toggle`, `cheatsheet-open`, or `cheatsheet-close` from a bind or `umbriel msg`.

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

With the Noctalia shell, route these keys through its IPC: the volume and brightness OSDs show, and the binds stay available while the session is locked.

```toml
"XF86AudioRaiseVolume" = { action = "spawn:noctalia msg volume-up", allow_when_locked = true }
"XF86AudioLowerVolume" = { action = "spawn:noctalia msg volume-down", allow_when_locked = true }
"XF86AudioMute" = { action = "spawn:noctalia msg volume-mute", allow_when_locked = true, repeat = false }
"XF86AudioPlay" = { action = "spawn:noctalia msg media toggle", allow_when_locked = true, repeat = false }
"XF86AudioNext" = { action = "spawn:noctalia msg media next", allow_when_locked = true }
"XF86AudioPrev" = { action = "spawn:noctalia msg media previous", allow_when_locked = true }
"XF86MonBrightnessUp" = { action = "spawn:noctalia msg brightness-up", allow_when_locked = true }
"XF86MonBrightnessDown" = { action = "spawn:noctalia msg brightness-down", allow_when_locked = true }
```

Without the shell, spawn the usual utilities directly:

```toml
"XF86AudioRaiseVolume" = "spawn:wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+"
"XF86AudioLowerVolume" = "spawn:wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"
"XF86AudioPlay" = "spawn:playerctl play-pause"
"XF86AudioNext" = "spawn:playerctl next"
"XF86AudioPrev" = "spawn:playerctl previous"
"XF86MonBrightnessUp" = "spawn:brightnessctl set +5%"
"XF86MonBrightnessDown" = "spawn:brightnessctl set 5%-"
```

Volume commands require `wpctl`, media commands require `playerctl`, and brightness commands require `brightnessctl`.
