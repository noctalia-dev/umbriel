# Behavior rules

Behavior keys change how matching windows interact with input, displays, and capture. They update while the window is open and on config reload.

```toml
# Variable refresh rate for game content
[[window_rule]]
match.content_type = "game"
vrr = "always"
```

## Behavior keys

| Key                       | Default | Description                                                                                                                 |
| ------------------------- | ------- | --------------------------------------------------------------------------------------------------------------------------- |
| `confine_pointer`         | `false` | Keep the pointer inside the window while it has keyboard focus. See [Games](#games).                                        |
| `allow_shortcuts_inhibit` | `true`  | Let the app take over compositor shortcuts. See [Keep compositor shortcuts](#keep-compositor-shortcuts).                    |
| `background_render`       | `false` | Keep drawing while hidden. See [Keep drawing while hidden](#keep-drawing-while-hidden).                                     |
| `block_capture`           | `false` | Leave the window out of screenshots and recordings. See [Hide a window from screenshots](#hide-a-window-from-screenshots).  |
| `focus_on_activate`       | global  | Override `general.focus_on_activate`. See [Focus on activation](#focus-on-activation).                                      |
| `touchpad_scroll_factor`  | unset   | Touchpad scroll speed multiplier, 0.1 to 10.0. See [Touchpad scroll speed](#touchpad-scroll-speed).                         |
| `vrr`                     | output  | Override the output's [VRR](display-features.md#variable-refresh-rate) policy: `"disabled"`, `"always"`, or `"fullscreen"`. |
| `tearing`                 | client  | Request (`true`) or veto (`false`) [tearing](display-features.md#tearing).                                                  |
| `hdr`                     | output  | Override the output's [HDR](display-features.md#hdr) policy.                                                                |

`vrr`, `tearing`, and `hdr` apply while the window is focused. A window rule's `vrr = "fullscreen"` follows the window's own fullscreen state.

## Games

```toml
# Keep the pointer in games that release it in menus
[[window_rule]]
match.app_id = "^steam_app_.*$"
confine_pointer = true
```

- Confinement applies while the window has keyboard focus and the pointer is inside it, tiled, floating, or fullscreen.
- Pointer locks requested by the game still work; confinement resumes when the game releases its lock.
- Escape with a focus or workspace keybind.
- It is suspended during session lock, the overview, and compositor move or resize.

```toml
# Variable refresh and tearing for Steam games
[[window_rule]]
match.app_id = "^steam_app_.*$"
vrr = "always"
tearing = true
```

Tearing also needs `tearing = true` on the output; see [Tearing](display-features.md#tearing).

## Keep compositor shortcuts

```toml
# Compositor shortcuts keep working in virtual machines
[[window_rule]]
match.app_id = "^virt-manager$"
allow_shortcuts_inhibit = false
```

Virtual machines and remote desktops normally capture every key. This denies their request; the [manual passthrough toggle](keybinds.md#keyboard-shortcuts-inhibition) still works.

## Keep drawing while hidden

```toml
# OBS keeps running on another workspace
[[window_rule]]
match.app_id = "^obs$"
background_render = true
```

A window that no output shows, on another workspace, in a hidden tab, or in a hidden scratchpad, is told it is suspended and can stop drawing. Applications that tie other work to drawing can stall. This keeps them running as if shown, at the cost of the CPU and GPU time they spend drawing.

## Hide a window from screenshots

```toml
# Keep a private app out of screenshots and recordings
[[window_rule]]
match.app_id = "^private-app$"
block_capture = true
```

- Output and region captures show what is underneath; no black box replaces the window.
- Direct capture of the window is refused, and enabling the rule stops existing sessions.
- Frames already recorded cannot be recalled.
- The window's popups and transients are protected too.
- Local display, input, focus, and layout are unchanged. Protected captures redraw the whole output.

> [!WARNING]
> While any X11 window is protected, X11 popups without a known parent are also left out of captures, which can hide unrelated X11 menus and notifications.

## Touchpad scroll speed

```toml
# Faster touchpad scrolling in a document viewer
[[window_rule]]
match.app_id = "^org[.]pwmt[.]zathura$"
touchpad_scroll_factor = 2.0
```

The factor multiplies [`input.touchpad.scroll_factor`](input.md#touchpad). Mouse wheels are unaffected.

## Focus on activation

`focus_on_activate` overrides [`general.focus_on_activate`](configuration.md#general) for matching windows: whether an application's activation request focuses and reveals the window.
