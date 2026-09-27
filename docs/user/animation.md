# Animation

Animation settings live under `[animation]`. The top-level values provide
defaults, and each event can override them.

```toml
[animation]
enabled = true
duration_ms = 250
curve = "easeout"

[animation.windows_in]
enabled = true
curve = "spring:1,900"
style = "popin"
scale = 0.5

[animation.windows_out]
enabled = true
curve = "spring:1,1400"
style = "popin"
scale = 0.8

[animation.windows_move]
enabled = true
curve = "spring:1,900"

[animation.workspaces]
enabled = true
curve = "spring:1,800"

[animation.overview]
enabled = true
curve = "spring:1,800"
workspace_curve = "spring:1,1000"

[animation.scratchpad]
enabled = true
curve = "spring:1,800"
dim = 0.8
blur = false
scale = 0.0
maximize = false
fullscreen = false

[animation.border]
enabled = true
curve = "spring:1,900"

[animation.dim_unfocused]
enabled = false
dim = 0.0

[animation.layers]
enabled = false
```

## Defaults

| Key | Default | Description |
| --- | --- | --- |
| `enabled` | `true` | Master switch for every transition. |
| `duration_ms` | `250` | Default duration for non-spring curves. |
| `curve` | `"easeout"` | Default easing curve. |

Each event other than `windows_drag` also accepts `enabled`, `duration_ms`,
and `curve`. A spring curve chooses its own duration, so `duration_ms` has no
effect on that event.

## Event tables

| Table | Additional fields | Transition |
| --- | --- | --- |
| `[animation.windows_in]` | `style`, `scale` | Window opening |
| `[animation.windows_out]` | `style`, `scale` | Window closing |
| `[animation.windows_move]` | none | Move, resize, reflow, maximize, and restore |
| `[animation.workspaces]` | none | Workspace switching |
| `[animation.overview]` | `workspace_curve` | Overview opening, closing, and filmstrip movement |
| `[animation.scratchpad]` | `dim`, `blur`, `scale`, `maximize`, `fullscreen` | Scratchpad windows and backdrop |
| `[animation.border]` | none | Focus-border color |
| `[animation.dim_unfocused]` | `dim` | Unfocused-window opacity |
| `[animation.layers]` | none | Layer-shell map and unmap |
| `[animation.windows_drag]` | `physics` (default `false`), its only key | Drag physics |

`windows_in` accepts `popin`, `zoom`, `slide`, `fade`, or `none`.
`windows_out` accepts `fade`, `slide`, `popin`, or `zoom`. `scale` applies to
`popin`: an opening window grows from it to full size, and a closing window
shrinks toward it, while both fade.

`animation.overview.workspace_curve` controls filmstrip movement after wheel,
keyboard, and touchpad navigation.

Scratchpad `dim` and `blur` remain active without a fade when animation is
disabled. `scale`, `maximize`, and `fullscreen` set the presentation applied
when a window enters a scratchpad.

## Curves

Use a built-in curve such as `linear`, `ease`, `easeout`, `snappy`, `bounce`, or
`elastic`; a cubic Bézier string; or a spring:

```toml
curve = "0.05,0.9,0.1,1.0"
# Or use a spring:
# curve = "spring:1,1000"
```

For Bézier curves, x coordinates must be between 0 and 1. Spring syntax is
`spring:<damping>,<stiffness>`:

- Damping below 1 overshoots.
- Damping 1 reaches the target without overshoot.
- Damping above 1 approaches more slowly.
- Greater stiffness settles faster.

Register reusable names when several events share a curve:

```toml
[animation.beziers]
myBezier = [0.05, 0.9, 0.1, 1.05]

[animation.springs]
myBounce = { damping = 0.5, stiffness = 200 }
```

Then set `curve = "myBezier"` or `curve = "myBounce"`.

## Custom effects

Every animation event other than `windows_drag` can run a custom program.
Define an `animation` preset and select it with `effect`; the event's enabled
state, duration, and curve still control its timeline. Umbriel ships `reveal`
and `squash`:

```toml
[include]
files = [
  "/usr/share/umbriel/effects/animation/reveal/effect.toml",
  "/usr/share/umbriel/effects/animation/squash/effect.toml",
]

[animation.windows_in]
duration_ms = 300
curve = "easeout"
effect = "reveal"

[animation.windows_out]
duration_ms = 250
curve = "easeout"
effect = "reveal"

[animation.windows_move]
effect = "squash"
```

Adjust `/usr` for the package prefix. Defining your own preset, the shader
interface, and reload behavior are in [Effects](effects.md). `windows_in` and
`windows_out` without an effect keep their built-in fade and `style`; with an
effect selected, `style` and `scale` are ignored. A running event keeps its
program; a reload affects the next event.

### Animation uniforms

| Name | Meaning |
| --- | --- |
| `umbriel_progress` | Eased progress, including overshoot |
| `umbriel_clamped_progress` | Eased progress clamped to 0 through 1 |
| `umbriel_linear_progress` | Progress before easing |
| `umbriel_direction` | `1` for entering and `-1` for leaving |
| `umbriel_random_seed` | Four stable random values for this transition |

### Targets

`windows_in`, `windows_out`, `windows_move`, and `dim_unfocused` process the
window and its subsurfaces as one target; `border` processes the ring alone.
`scratchpad` covers the window's show and hide fade and the dim and blur
backdrops. `layers` covers a layer-shell surface's own tree. `workspaces` and
`overview` process whole workspace or overview trees, so they see the results
of inner effects. Effects composite descendants before ancestors.

## Drag physics

```toml
[animation.windows_drag]
physics = true
```

With drag physics on, a window dragged with the pointer bends like an elastic
sheet pinned under the pointer, trails its motion, and settles when released
or held still. It needs the animation master switch. Border, window, and
overlay effects keep rendering on the deformed window. A window closed
mid-drag keeps its shape while it fades.

Only the window's own content deforms; its drop shadow follows that
deformation within the window's shadow bounds. Re-grabbing a window while it
settles continues its motion. Under the default `popin` style, or under
`zoom`, the closing snapshot's clip grows by the deformation margin, so a
client-side decoration extending past the window's geometry can remain
visible within that margin while the snapshot fades.
