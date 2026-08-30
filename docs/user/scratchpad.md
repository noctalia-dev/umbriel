# Scratchpads

A scratchpad is a holding area for windows you want nearby without keeping them
on a workspace. Each output supports multiple dedicated named scratchpad slots (or
a default slot), and windows stored in a slot can be shown, hidden, or restored
together.

Scratchpad membership and visibility are separate:

- Moving a window to a scratchpad slot stores it there.
- Toggling a scratchpad slot shows or hides its stored windows.
- Restoring a window removes it from the scratchpad slot and returns it to a
  workspace.

## Basic setup

The packaged config uses these bindings:

```toml
[keybinds]
"Mod+Shift+Space" = "window-move-to-scratchpad"
"Mod+Space" = "scratchpad-toggle"
"Mod+Ctrl+Space" = "window-restore-from-scratchpad"
"Mod+Tab" = "scratchpad-focus-next"

# Named drawers
"Mod+M" = "scratchpad-toggle:music"
"Mod+Shift+M" = "window-move-to-scratchpad:music"
"Mod+Ctrl+Shift+M" = "window-move-to-scratchpad-silent:music"
```

A typical workflow is:

1. Focus a workspace window and press `Mod+Shift+Space` to store it.
2. Press `Mod+Space` to show the stored windows.
3. Press `Mod+Tab` to cycle focus when several windows are visible in the slot.
4. Press `Mod+Ctrl+Space` to return the focused window to its workspace.

Press `Mod+Space` instead of restoring when you only want to hide the
scratchpad again.

## Actions

| Action | What it does |
|--------|--------------|
| `window-move-to-scratchpad` | Move the focused workspace window into the target scratchpad slot. |
| `window-move-to-scratchpad-silent` | Move the focused window into the target slot without transferring focus away from the active workspace. |
| `scratchpad-toggle` | Show or hide windows in the target scratchpad slot. |
| `window-restore-from-scratchpad` | Restore the focused scratchpad window to its original workspace. |
| `window-toggle-scratchpad` | Move the focused window into the scratchpad, or restore it if already focused in a slot. |
| `scratchpad-focus-next` | Focus the next visible window in the active scratchpad slot. |

The toggle, restore, and focus actions do nothing when their required window is
not available. In particular, restore and focus-next require the slot to be
visible.

Scratchpad visibility and cycling actions never repeat while their key is held,
even if the binding does not set `repeat = false`.

For compatibility with special workspace workflows, `workspace` and
`window-move-to-workspace` also accept a `special:<name>` selector, which is
equivalent to `scratchpad-toggle:<name>` and `window-move-to-scratchpad:<name>`.

## Choosing a slot and an output

The argument is `<name>`, `<name>/<output>`, or just `<output>`:

- `scratchpad-toggle` — default slot, output under the pointer.
- `scratchpad-toggle:music` — the `music` slot, output under the pointer.
- `scratchpad-toggle:music/DP-1` — the `music` slot on `DP-1`.
- `scratchpad-toggle:DP-1` — a bare argument that matches a connected output
  name is read as an output, so older output-only bindings keep working. A slot
  cannot share a name with an output.

If the pointer is outside every output, Umbriel uses the first enabled output.
Use `umbriel outputs` inside a session to find output names such as `DP-1` or
`HDMI-A-1`.

If a slot is visible on one output and you toggle it on another, Umbriel hides
it on the first output, moves its windows, and shows it on the second.

## Visibility and focus

Showing a scratchpad focuses the window that was most recently focused there.
If no window has been focused yet, Umbriel focuses the first stored window.
Hiding it returns focus to a regular workspace window.
Opening the workspaces overview immediately hides every visible scratchpad
while keeping its windows stored.

## Routing windows with window rules

`default_scratchpad` (or `scratchpad`) stores a window in a slot as it opens,
without showing the slot:

```toml
[[window_rule]]
match.app_id = "^Spotify$"
default_scratchpad = "music"

[[window_rule]]
match.title = ".*btop.*"
default_scratchpad = "monitor"
```

## Touchpad gestures

A four-finger vertical swipe toggles at the end of the gesture, based on how far
and how fast you swiped. It does not track the drawer to your finger.

- **Swipe down**: shows the default scratchpad slot.
- **Swipe up**: opens the workspace overview, or closes the visible scratchpad
  slot if one is open.

## Several windows in one slot

A slot tiles its windows with the same layout engine a workspace uses, inside
the slot's box. By default that is `layout.mode`; `layout` in a `[[scratchpad]]`
rule overrides it per slot, and `gap` overrides `layout.gap`. Closing or
restoring a window re-tiles the rest. Use `scratchpad-focus-next` to cycle
across them.

## Empty slots

Toggling a slot that holds no windows opens it anyway: the backdrop appears and
the slot stays addressable, so the same binding closes it again. This is when an
`on_empty` command runs. A slot is discarded once it is both hidden and empty.

## Configuration

Defaults for every slot live under `[animation.scratchpad]`:

```toml
[animation.scratchpad]
enabled = true
duration_ms = 250
curve = "easeout"              # "easeout", "snappy", cubic-bezier, or spring
dim = 0.5                      # backdrop dim over the workspace, 0.0-1.0
blur = false                   # backdrop blur (needs appearance.blur.enabled)
scale = 0.8                    # fraction of the usable area, 0.1-1.0
direction = "top"              # edge the slot slides in from ("top", "bottom", "left", "right")
style = "slide"                # "slide", "fade", "popin", or "slidefade"
suspend_hidden = true          # suspend clients while the slot is hidden
maximize = false               # fill the usable area instead of scaling
maximize_to_edges = false      # fill screen edge-to-edge without gaps or borders
fullscreen = false             # fill the whole output instead of scaling
```

`scale` sets both the width and height fraction of the output's usable area,
centred. `maximize` fills the usable area; `maximize_to_edges` fills the entire
usable area to the monitor edges; `fullscreen` fills the whole output,
ignoring layer-shell reservations. Neither changes the window's xdg-shell
maximized or fullscreen state — they only size the slot.

`style` picks the show/hide transition: `"slide"` moves the slot in from
`direction` at full opacity (the default); `"slidefade"` does the same slide
while also fading opacity in/out; `"popin"` zooms the slot in/out from a
centered, scaled-down box instead of sliding, with a fade. `"fade"` and
`style` are ignored when `scale` is `0` — an unscaled slot always fades in
place at its existing geometry.

### Per-slot rules

`[[scratchpad]]` overrides the defaults for one slot:

```toml
[[scratchpad]]
name = "music"
scale = 0.65
direction = "right"
duration_ms = 350
curve = "easeout"
dim = 0.50
blur = true
on_empty = "spotify"

[[scratchpad]]
name = "terminal"
scale = 0.85
direction = "top"
duration_ms = 220
curve = "snappy"
on_empty = "kitty"
layout = "master"
gap = 12
```

- **`name`** (required string): slot identifier. `special:` is stripped, so
  `"special:music"` and `"music"` are the same slot. `""` is the default slot.
- **`scale`** (`0.1`–`1.0`), **`direction`** (`"top"`, `"bottom"`, `"left"`,
  `"right"`), **`duration_ms`** (`1`–`10000`), **`curve`**, **`dim`**
  (`0.0`–`1.0`), **`blur`**, **`maximize`**, **`maximize_to_edges`**,
  **`fullscreen`**, **`suspend_hidden`**: as above, for this slot only.
- **`layout`** (string): `"scrolling"`, `"dwindle"`, or `"master"` for the
  windows inside this slot. Defaults to `layout.mode`.
- **`gap`** (integer `0`–`200`): gap between windows in this slot. Defaults to
  `layout.gap`.
- **`on_empty`** (string, also accepted as `on_created_empty`): command to run
  when the slot is toggled while empty. Only a window whose process descends
  from that command is placed in the slot, so launching something else
  meanwhile is safe. The claim expires after 30 seconds.

When several slots are visible across outputs, the backdrop uses the largest
`dim` of the visible slots on that output, and blurs if any of them asks for it.

### Suspending hidden clients

With `suspend_hidden = true`, Umbriel sends `xdg_toplevel.suspended` to clients
in a hidden slot (xdg-shell version 6 and up). Clients that honour it — browsers,
Electron apps, some media players — throttle or stop rendering while hidden.
Clients that ignore it keep running as before. Hidden windows are also removed
from the scene graph, so they cost nothing to composite either way.

## Restoring windows

Umbriel remembers the source output, workspace, and whether the window was
tiled or floating. Restoring the window returns it to that workspace and
restores its tiled or floating state.

If the original output no longer exists, Umbriel restores the window on the
output targeted by the action. If the original workspace no longer exists, it
uses that output's active workspace.

Fullscreen, pinned, and maximize-to-edges state are cleared when a window enters
the scratchpad and are not restored automatically.

## Moving scratchpad windows

Scratchpad windows always float. Dragging one does not restore it or tile it on
the workspace beneath it.

Dragging a scratchpad window to another output assigns it to that output's
scratchpad and makes the destination slot visible.

When an output disconnects or is disabled, its scratchpad windows move to
another enabled output. A visible slot remains visible after that move. When the
original output returns, its scratchpad windows return with their output-relative
positions intact.

## Appearance and window actions

Scratchpad windows use dedicated focused and unfocused border colors:

```toml
[appearance]
scratchpad_border_focused = "#E5C07BFF"
scratchpad_border_unfocused = "#5C4A2AFF"
```

See [Appearance](appearance.md) for the complete appearance reference, and
[`animation.scratchpad`](animation.md#animation) for the transition settings.

While a scratchpad window has focus, `window-toggle-floating`,
`window-toggle-pinned`, and `window-center` are inactive. Restore the window
before using those actions.

## Troubleshooting

- If a slot opens empty, its `on_empty` command is missing, failed to start, or
  the app it launched has not mapped a window yet.
- If restore or focus-next does nothing, show the target slot first.
- If an action affects the wrong monitor, move the pointer to the intended
  output or add an explicit `/<output>` suffix.
- If a window will not tile, pin, or center, restore it to a workspace first.
