# Effects

Effects are GLSL programs Umbriel runs on animation events, on the focused
window's border, on windows, on whole outputs, and around the pointer. Every
effect is off until you select one. Umbriel ships a small set; each is a
preset you include and then name where it should apply.

## Use a bundled effect

Bundled presets install under `share/umbriel/effects/<kind>/<name>/`. Include a
preset's `effect.toml`, then select its name. For an installation under `/usr`:

```toml
[include]
files = [
  "/usr/share/umbriel/effects/border/pulse/effect.toml",
  "/usr/share/umbriel/effects/animation/reveal/effect.toml",
]

[effects]
border = "pulse"

[animation.windows_in]
effect = "reveal"
```

Including a file only makes its preset available. The `border` and `effect`
selectors are what turn it on. If your configuration already has an
`[include]` table, append the paths to its `files` array and add the selectors
to your existing `[effects]` and `[animation.windows_in]` tables rather than
repeating the tables.

Bundled presets:

| Preset | Kind | Selector |
| --- | --- | --- |
| `reveal` | animation | `[animation.windows_in] effect = "reveal"` (also `windows_out`) |
| `squash` | animation | `[animation.windows_move] effect = "squash"` |
| `pulse` | border | `[effects] border = "pulse"` |
| `scanlines` | window | `[effects] window = "scanlines"` |
| `vignette` | screen | `[effects] screen = "vignette"` |
| `glow` | cursor | `[effects] cursor = "glow"` |

## Turn a default off for one window or output

Set a selector to `""` to select nothing. A window rule or an output table can
replace the default by name or switch it off with `"off"`:

```toml
[effects]
border = "pulse"
screen = "vignette"

[[window_rule]]
match.app_id = "^mpv$"
border_effect = "off"
window_effect = "scanlines"

[output."HDMI-A-1"]
screen_effect = "off"
```

`border_effect` and `window_effect` follow the usual window-rule merging: the
last matching rule that sets a key wins. The cursor effect has no per-window
or per-output override.

## Settings

`[effects]`:

| Key | Default | Description |
| --- | --- | --- |
| `border` | `""` | Border preset for the focused window. |
| `window` | `""` | Window preset applied to every window. |
| `screen` | `""` | Screen preset applied to every output. |
| `cursor` | `""` | Cursor preset. |
| `max_fps` | `0` | Cap, 0 to 240, for frames drawn only because an effect animates. `0` follows each output's refresh rate. |
| `in_capture` | `false` | Include window, screen, and cursor effects in screencopy and image-copy captures. Border effects always appear, and an export-dmabuf capture always sees the same frame as the display, regardless of this setting. |

Where each kind draws:

- **animation** binds to an animation event through `[animation.<event>]
  effect = "<name>"`. The event's `enabled`, `duration_ms`, and `curve` still
  own its timeline; see [Animation](animation.md#custom-effects).
- **border** draws on the focused window's border ring while the window is
  decorated, not fullscreen, and not urgent. `padding` reserves transparent
  space around the ring for the effect to paint into.
- **window** draws over each window in place, regardless of focus, including
  undecorated and fullscreen windows. It follows the window through opening,
  closing, moving, workspace switches, and the overview. A floating window
  with client-side decorations and `corner_radius = 0` has no rounding to mask
  against, so the effect also shades the transparent margin the client draws
  around such a window.
- **screen** draws over the whole output after everything else, except the
  cursor effect and the software cursor.
- **cursor** draws in a square of `radius` logical pixels around the pointer
  (`0` covers the whole output), after the screen effect, only on the output
  currently holding the pointer, clipped at that output's edges. It runs
  before the software cursor is drawn and never shades the cursor image, and
  never affects a hardware cursor. It hides when the compositor hides the
  pointer; a client that hides its own cursor image does not by itself turn
  the effect off.

The session lock detaches screen and cursor effects and never shades the lock
surface.

## Define a preset

`[effects.preset.<name>]` defines one preset. `kind` is required; `shader` is
a GLSL file relative to the TOML file that names it. The file is watched and
reloads with the configuration; a missing or unreadable file reports a
diagnostic and leaves the preset inert until the file appears, and a preset
without `shader` is inert as well. `off` is a reserved name.

| Key | Kinds | Default | Description |
| --- | --- | --- | --- |
| `kind` | all | required | `animation`, `border`, `window`, `screen`, or `cursor`. |
| `shader` | all | none | Path to the GLSL source, at most 256 KiB. Without it the preset is inert. |
| `palette` | all | `false` | Supply `[colors]` accent and status colors to the program. |
| `padding` | border | `0` | Transparent space around the ring the effect may paint, 0 to 1024. |
| `speed` | border | `1.0` | Multiplier on `umbriel_time`, 0 to 10. `0` holds `umbriel_time` at zero. |
| `animated` | border | `true` | `false` freezes `umbriel_time` at zero. |
| `overlay` | border | `""` | A window preset drawn on the window while the border effect applies. |
| `light.spread` | border | `80` | How far light from the ring spills, 1 to 256 logical pixels. Defining `[effects.preset.<name>.light]` enables light. |
| `light.intensity` | border | `1.0` | Light gain, 0 to 4. |
| `light.threshold` | border | `0.5` | Brightness a ring pixel needs before it emits, 0 to 1. |
| `radius` | cursor | `0` | Half-size of the square around the pointer, 0 to 4096; `0` covers the output. |

Border light is built from the ring in buffer pixels, so the same preset's
brightness differs across output scales. The light itself stacks below panels
and pinned windows, above a window being dragged.

Keys that do not belong to a preset's kind are reported as unknown. Defining
the same preset name in two files is an error.

```toml
[effects.preset.tint]
kind = "window"
shader = "tint.glsl"
palette = true

[effects]
window = "tint"
```

## Write a shader

Sources are GLSL ES 1.00 fragment code without `#version`, `main`, or precision
qualifiers. Each kind defines one entry point that receives `uv`, normalized
over the drawn rectangle with `(0, 0)` at the top left, and returns
premultiplied RGBA:

| Kind | Entry point |
| --- | --- |
| animation | `vec4 animation(vec2 uv)` |
| border | `vec4 border(vec2 uv)` |
| window | `vec4 window(vec2 uv)` |
| screen | `vec4 screen(vec2 uv)` |
| cursor | `vec4 cursor(vec2 uv)` |

Every kind sees:

| Name | Meaning |
| --- | --- |
| `umbriel_sample(vec2 uv)` | The input under the drawn rectangle: the captured window for animations, the native ring for borders, the pixels already on screen for window, screen, and cursor effects. |
| `umbriel_sample_previous(vec2 uv)` | This effect's previous result. Using it allocates two extra buffers for each window or output it runs on. |
| `umbriel_size` | Drawn width and height in logical pixels. |
| `umbriel_scale` | Buffer pixels per logical pixel. |
| `umbriel_expand` | How far the drawn rectangle extends past the window on each side, as a fraction of its width and height. `(0, 0)` except for an animation running while drag physics deforms the window. |
| `umbriel_time` | Seconds on the animation clock, times the border's `speed`. Held as a single-precision float that is never wrapped, so fine time-based motion loses precision after long uptimes. |
| `umbriel_palette_count` | `4` for palette presets, `0` otherwise. |
| `umbriel_palette_at(float t)` | The palette color at `t`, blended between neighboring colors from the wrapping sequence `accent_primary`, `accent_secondary`, `warning`, `error`. Transparent black when there is no palette. |

Animations add `umbriel_progress`, `umbriel_clamped_progress`,
`umbriel_linear_progress`, `umbriel_direction`, and `umbriel_random_seed`
([Animation](animation.md#custom-effects)). Borders add `umbriel_border_hole`
(the client rectangle in `uv`), `umbriel_border_radius` (its corner radii in
logical pixels), and `umbriel_border_distance(vec2 uv)`, the signed distance
in logical pixels to the client rectangle, negative inside it; the client hole
is always cut out of a border's result. Cursor effects add `umbriel_pointer`,
the pointer position in `uv`.

### What a window effect sees

At rest, a window effect reads the output framebuffer after the window has been
drawn, so through a translucent window it sees and may rewrite the desktop
behind it. While an animation encloses the window (opening, closing, moving,
a workspace switch, the overview, or drag physics) it reads that animation's
capture instead: it shades the window's own content, and the result is
composited over the live desktop. A shader that depends on the backdrop must
tolerate that change when an animation begins or ends. A border's `overlay`
follows the same rule.

### Reload and failures

Presets compile at startup and on reload. A compile error is logged with the
preset's name and the driver's message, whose line numbers count from the top
of the shader file; that preset renders plainly (opening and closing
animations keep their built-in animation, `style` and `scale` included) until
a reload fixes it. Unknown names, or a preset of the wrong kind for a
selector, report a diagnostic and are dropped: a top-level `[effects]`
selector selects nothing, and a window rule's or output's own override falls
back to an earlier matching rule or the `[effects]` default. Shaders are
trusted local GPU code; keep them small and side-effect free.

## Cost

Nothing here costs anything until selected. A border effect renders the ring
through a capture and one program pass per frame on the focused window, and
requests extra frames only while its program reads `umbriel_time` and its
clock advances, capped by `max_fps`. Light adds a second program pass and a
blurred pyramid where the ring draws, and a blend on every output its light
reaches. A window effect copies the pixels under the window and runs one pass
per window per frame. Screen and cursor effects each run one pass over the
output or the radius square and disable direct scanout on that output. Drag
physics costs only while a window is held or settling.
With `in_capture = false`, a pending screencopy or image-copy capture of an
output composes its frame twice whenever any window, screen, or cursor effect
is visible on that output.
