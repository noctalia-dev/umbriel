# Animation

Animation settings live in the top-level `animation` section. `duration_ms` and
`curve` set defaults for every event when present; a nested event can override
either value. The master switch makes every transition instant. Each event also
has its own switch.

```toml
[animation]
enabled = true
duration_ms = 250
curve = "easeout"

[animation.windows_in]
enabled = true
duration_ms = 150
curve = "easeout"
style = "popin"       # "popin", "zoom", "slide", "fade", or "none"
scale = 0.85          # 0.1-1.0, used by "popin"

[animation.windows_out]
enabled = true
duration_ms = 150
curve = "easeout"
style = "fade"        # "fade" or "slide"

[animation.windows_move]
enabled = true
duration_ms = 250
curve = "snappy"

[animation.workspaces]
enabled = true
duration_ms = 250
curve = "easeout"

[animation.overview]
enabled = true
duration_ms = 250
curve = "easeout"

[animation.scratchpad]
enabled = false
duration_ms = 250
curve = "easeout"
dim = 0.5             # 0.0-1.0
blur = false          # requires appearance.blur.enabled
scale = 0.0           # 0 preserves geometry; 0.1-1.0 sizes and centers on entry
maximize = false      # maximize to edges on entry
fullscreen = false    # fullscreen on entry

[animation.border]
enabled = false
duration_ms = 250
curve = "easeout"

[animation.dim_unfocused]
enabled = false
duration_ms = 250
curve = "easeout"
dim = 0.0             # 0.0-1.0

[animation.layers]
enabled = false
duration_ms = 250
curve = "easeout"
```

## Defaults

| Key           | Type   | Default   | Description                                                                 |
| ------------- | ------ | --------- | --------------------------------------------------------------------------- |
| `enabled`     | bool   | `true`    | Master switch. When false, every animation transition is instant.           |
| `duration_ms` | int    | `250`     | Default duration for all events when explicitly set (1-10000 milliseconds). |
| `curve`       | string | `easeout` | Default curve for all events when explicitly set.                           |

## Event tables

Each event table accepts `enabled`, `duration_ms`, and `curve`. The following
fields are specific to individual event tables:

| Table                      | Additional fields                                                                                                  | Transition                                          |
| -------------------------- | ------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------- |
| `[animation.windows_in]`   | `style` (`popin`, `zoom`, `slide`, `fade`, or `none`); `scale` (0.1-1.0, for `popin`)                            | Window open.                                        |
| `[animation.windows_out]`  | `style` (`fade` or `slide`)                                                                                       | Window close, using a scene snapshot.               |
| `[animation.windows_move]` | None                                                                                                               | Window move, resize, and floating maximize transitions. |
| `[animation.workspaces]`   | None                                                                                                               | Workspace switch.                                   |
| `[animation.overview]`     | None                                                                                                               | Overview open, close, and row settling.             |
| `[animation.scratchpad]`   | `dim` (0.0-1.0); `blur`; `scale` (0.0-1.0); `maximize`; `fullscreen`                                             | Scratchpad show, hide, and backdrop.                |
| `[animation.border]`       | None                                                                                                               | Focus-ring color transition in OkLab color space.   |
| `[animation.dim_unfocused]` | `dim` (0.0-1.0)                                                                                                 | Unfocused-window opacity. `dim = 0` disables it.    |
| `[animation.layers]`       | None                                                                                                               | Layer-shell surface map and unmap fades.            |

An event's `enabled = false` makes only that transition instant. Scratchpad
`dim` and `blur` remain active, without a fade, when animation is disabled.
Scratchpad `scale`, `maximize`, and `fullscreen` apply when a window enters the
scratchpad.

## Curves

Each curve accepts a built-in name such as `linear`, `ease`, `easeout`,
`snappy`, `bounce`, or `elastic`; a cubic bezier string
`"x1,y1,x2,y2"`; or a spring string `"spring: damping,stiffness"`. Bezier x
coordinates must be between 0 and 1. Spring damping must be between 0.01 and 5,
and stiffness between 1 and 1000.

Custom named curves can be registered once and reused by name:

```toml
[animation.beziers]
myBezier = [0.05, 0.9, 0.1, 1.05]

[animation.springs]
myBounce = { damping = 0.5, stiffness = 200 }
```

Then reference them as `curve = "myBezier"` or `curve = "myBounce"` in any
event section.

## Custom GLSL shaders

Every event section supports a custom fragment shader: `windows_in`,
`windows_out`, `windows_move`, `workspaces`, `overview`, `scratchpad`, `border`,
`dim_unfocused`, and `layers`. Existing configurations keep their built-in
animations. The event's `enabled`, `duration_ms`, and `curve` still control its
timeline.

Umbriel ships `reveal.glsl` and `squash.glsl` in the installed data directory's
`umbriel/shaders/` directory. **No copying is required.** These are bundled
shader files, not effects selected by name.

`reveal.glsl` reveals or hides a window. `squash.glsl` adds a subtle, smooth
compression and return during movement or resizing, without rippling. It keeps
the window centered, compresses its width by up to 2% and height by up to 4.5%,
and leaves both endpoints unchanged.

For a package installed under `/usr`, reference the bundled files directly:

```toml
[animation.windows_in]
duration_ms = 300
shader = "/usr/share/umbriel/shaders/reveal.glsl"

[animation.windows_out]
duration_ms = 250
shader = "/usr/share/umbriel/shaders/reveal.glsl"

[animation.windows_move]
shader = "/usr/share/umbriel/shaders/squash.glsl"
```

Adjust `/usr/share` to your package's data directory, for example
`/usr/local/share` for a default local installation.

With the Umbriel Home Manager module on NixOS, derive the path from your
configured package instead of hardcoding a Nix store path. This assumes
`programs.umbriel.package` is set to your Umbriel package:

```nix
{ config, ... }:
let
  shaders = "${config.programs.umbriel.package}/share/umbriel/shaders";
in
{
  programs.umbriel.settings.animation = {
    windows_in = {
      duration_ms = 300;
      shader = "${shaders}/reveal.glsl";
    };
    windows_out = {
      duration_ms = 250;
      shader = "${shaders}/reveal.glsl";
    };
    windows_move.shader = "${shaders}/squash.glsl";
  };
}
```

If you want editable copies, copy the repository's `examples/shaders/` directory
beside your config and use paths such as `"shaders/reveal.glsl"` instead.
Relative paths resolve against the config file containing the setting, including
an included theme config. Absolute paths are also accepted. Shader-file edits trigger
the normal automatic config reload; `umbriel msg config-reload` also re-reads
them. Shell/environment expansion and recursive GLSL includes are not supported.

The `shader` setting accepts only a file path, not inline GLSL. Omit it to use
the built-in animation. Files must be regular, contain nonblank GLSL without
NUL bytes, and fit within 256 KiB.

For example, save this as `shaders/border-pulse.glsl` beside your config:

```glsl
vec4 animation(vec2 uv) {
    vec4 color = umbriel_sample(uv);
    float pulse = 0.3 * sin(3.14159265 * umbriel_clamped_progress);
    vec3 highlight = vec3(1.0, 0.4, 0.1) * color.a;
    return vec4(mix(color.rgb, highlight, pulse), color.a);
}
```

Then reference it:

```toml
[animation.border]
enabled = true
shader = "shaders/border-pulse.glsl"
```

### Shader interface

Write GLSL ES 1.00 with the entry point `vec4 animation(vec2 uv)`. Umbriel supplies
`main`, precision declarations, and the following interface. Do not include a
`#version` declaration or your own `main`.

| Name | Meaning |
| --- | --- |
| `uv` | Normalized target coordinates: top-left `(0, 0)`, bottom-right `(1, 1)`, independent of output rotation |
| `umbriel_sample(vec2 uv)` | Sample the rendered target; coordinates outside the target/output return transparent black |
| `umbriel_size` | Current target bounding width and height in logical units |
| `umbriel_progress` | Eased transition progress, normally 0 to 1; springs and overshooting curves may exceed that range |
| `umbriel_clamped_progress` | Eased progress clamped to 0 to 1 |
| `umbriel_linear_progress` | Timeline fraction before easing |
| `umbriel_direction` | `1` for opening/showing/focusing, `-1` for closing/hiding/unfocusing; movement uses the sign of its controlling scalar, overview settling uses `1` |

Progress always advances from the transition's start toward its end, including
when closing. For a shared show/hide shader, use
`umbriel_direction > 0.0 ? umbriel_clamped_progress : 1.0 - umbriel_clamped_progress`
as the amount visible. Shaders run only during transitions and are removed at
completion, so their endpoint must match the normal visible or hidden state.

Return premultiplied RGBA. Preserve the sampled alpha when modifying colors,
especially for borders: a shader returning an opaque constant can fill the
otherwise transparent center of a border's rectangular target. Samples use the
compositor's working color space: gamma 2.2 for ordinary SDR composition and
linear light when the color-management pass uses an FP16 intermediate target.
There is no previous-frame sampler.

### Targets and composition

Window open/close shaders replace the built-in fade/scale/slide style. A custom
opening shader also works with `style = "none"`. Layer and scratchpad window
show/hide shaders replace their built-in window fade. Movement/resize,
workspace, overview, border-color, dimming, and backdrop transitions retain
their native presentation changes and apply the shader to that presentation.
Shaders never change layout, client configure sizes, input coordinates, or focus.

A window's scene subtree, including its subsurfaces and border, is processed as
one target. Borders can also have their own inner effect. Workspace effects
process the output's workspace view root, and overview effects process each
output's overview tree. Scratchpad backdrops have their own targets. Compositor
shadows kept in a separate stacking layer retain their existing native behavior;
they are not relocated into window textures.

Child effects run before parent effects. Effects sharing a target run in this
order: dimming, border, movement, window opening, window closing, scratchpad,
layers, workspaces, overview. Thus an overview shader can sample a window's
already-processed pixels. Sampling and drawing remain bounded by the target and
its output/ancestor clips.

### Reload and failures

Programs are compiled on startup/config reload and cached per event, source,
and renderer. Active transitions retain their program until completion or
retargeting; disabling/removing an effect cancels its shader. Close snapshots
retain the preceding effect parameters when a window closes mid-transition.
GPU renderer recreation recompiles configured programs.

Missing sources and GLSL compilation failures produce diagnostics and use the
built-in effect. Compiler details and the shader file label appear in Umbriel's
log. If a render target cannot be allocated/imported, Umbriel draws the normal
presentation rather than dropping the window. Intermediate textures are
allocated only while needed and released after effects finish.

Custom GLSL is trusted local GPU code, not sandboxed code. Expensive or
nonterminating shaders can stall a driver. Effects add offscreen rendering work;
opaque-region culling and direct scanout are suspended while custom effects are
active. Prefer short transitions and inexpensive shaders.
