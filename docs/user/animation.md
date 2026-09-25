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

Each event also accepts `enabled`, `duration_ms`, and `curve`. A spring curve
chooses its own duration, so `duration_ms` has no effect on that event.

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

## Custom GLSL shaders

Every animation event can use a custom fragment shader. The event's enabled
state and curve still control its timeline.

Umbriel ships `reveal.glsl` and `squash.glsl`. Reference the installed files
directly:

```toml
[animation.windows_in]
duration_ms = 300
curve = "easeout"
shader = "/usr/share/umbriel/shaders/reveal.glsl"

[animation.windows_out]
duration_ms = 250
curve = "easeout"
shader = "/usr/share/umbriel/shaders/reveal.glsl"

[animation.windows_move]
shader = "/usr/share/umbriel/shaders/squash.glsl"
```

Adjust `/usr/share` for the package prefix. Relative paths resolve from the
configuration file containing the setting. Shader files are watched and reload
with the configuration.

NixOS users can derive the path from the configured package:

```nix
{
  programs.umbriel.settings.animation.windows_in.shader =
    "${config.programs.umbriel.package}/share/umbriel/shaders/reveal.glsl";
}
```

The `shader` value must name a regular GLSL file smaller than 256 KiB. Inline
GLSL and recursive includes are not supported.

### Shader interface

Write GLSL ES 1.00 with this entry point. Do not add a `#version` declaration
or your own `main`:

```glsl
vec4 animation(vec2 uv) {
    return umbriel_sample(uv);
}
```

Umbriel supplies `main`, precision declarations, and these commonly used
values:

| Name | Meaning |
| --- | --- |
| `uv` | Normalized target coordinates |
| `umbriel_sample(vec2 uv)` | Sample the rendered target |
| `umbriel_sample_previous(vec2 uv)` | Sample this target's previous shader result |
| `umbriel_size` | Target width and height in logical units |
| `umbriel_progress` | Eased progress, including overshoot |
| `umbriel_clamped_progress` | Eased progress clamped to 0 through 1 |
| `umbriel_linear_progress` | Progress before easing |
| `umbriel_direction` | `1` for entering and `-1` for leaving |
| `umbriel_random_seed` | Four stable random values for this transition |

Return premultiplied RGBA. Preserve sampled alpha when modifying colors so a
shader does not fill transparent parts of its target.

`umbriel_sample_previous` enables feedback and allocates two additional buffers
for the active target. Avoid it when an effect does not need feedback,
especially for workspace and overview shaders.

### Targets and composition

Window shaders process the window, subsurfaces, and border as one target.
Workspace and overview shaders process their corresponding scene trees.
Shaders change presentation only; they do not affect layout, client sizes,
input coordinates, or focus.

Window shadows follow the alpha shape produced by window and border shaders.
The compositor still applies configured color, softness, and offset.

### Reload and failures

Shaders compile on startup or configuration reload. A missing source or compile
failure produces a diagnostic and falls back to the built-in effect. Compiler
details appear in the Umbriel log.

Custom shaders are trusted local GPU code. Expensive or nonterminating shaders
can stall the driver, and active effects disable direct scanout. Prefer short,
inexpensive effects.
