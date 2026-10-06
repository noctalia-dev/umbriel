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
| `spectrum` | border | `[effects] border = "spectrum"`; requires the optional audio helper |
| `border.music-lines` | border + overlay | Fixed cyan/blue/magenta spectrum bars and mirrored contours whose heights follow the live audio bands; include `border/music-lines/effect.toml` |
| `scanlines` | window | `[effects] window = "scanlines"` |
| `vignette` | screen | `[effects] screen = "vignette"` |
| `glow` | cursor | `[effects] cursor = "glow"` |
| `trail` | cursor | Noctalia-inspired purple, lavender and moon-yellow motion tail; include `cursor/trail/effect.toml`, then set `[effects] cursor = "trail"`. |
| `trail-path` | cursor | Two-second curved tail coloured along its length; include `cursor/trail-path/effect.toml`, then set `[effects] cursor = "trail-path"`. |
| `cursor.music-radiance` | cursor | Expanding rainbow rings driven by playback; include `cursor/music-radiance/effect.toml` |
| `window.music-smoke` | window | Translucent rising smoke driven by playback; include `window/music-smoke/effect.toml` |

The `spectrum` preset explicitly follows system playback and responds without a
shader clock. Including its file does not start acquisition; selecting it does.
See [audio inputs](audio-effects.md) for source selection and helper setup.

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
| `border` | `""` | Border preset or pool for the focused window. |
| `window` | `""` | Window preset or pool applied to every window. |
| `screen` | `""` | Screen preset or pool applied to every output. |
| `cursor` | `""` | Cursor preset or pool, shared across outputs. |
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
  the effect off. Motion-history shaders expand the rectangle to cover the
  retained path, with `radius` padding.

The session lock detaches screen and cursor effects and never shades the lock
surface.

## Define a preset

`[effects.preset.<name>]` defines one preset. `kind` is required; `shader` is
a GLSL file relative to the TOML file that names it. The file is watched and
reloads with the configuration; a missing or unreadable file reports a
diagnostic and leaves the preset inert until the file appears, and a preset
without `shader` is inert as well.

Presets and pools share a namespace. Names must be non-empty, cannot be `off`,
and cannot contain `/`, which separates an action selector from its target. A
colliding pool is rejected while the preset remains valid, with diagnostics
pointing to both declarations.

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
| `radius` | cursor | `0` | Padding around the pointer (or retained motion path), 0 to 4096; `0` covers the output. |

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

## Pools

A pool chooses a stable preset for each owner before its first rendered frame:

```toml
[effects.pool.borders]
kind = "border"
choose = ["pulse", "quiet"]
selection = "unused_first"

[effects]
border = "borders"
```

Define or include the `pulse` and `quiet` border presets separately.

| Key | Required/default | Meaning |
| --- | --- | --- |
| `kind` | Required | `border`, `window`, `screen`, or `cursor`. |
| `choose` | Required array | Ordered names of presets of that kind. |
| `selection` | `unused_first` | `unused_first`, `round_robin`, or `random`. |

`unused_first` chooses the least-held member, breaking ties in list order.
`round_robin` hands out members in order and wraps. `random` makes a uniform
random choice. Invalid or duplicate members are dropped individually, keeping
the first valid occurrence. A missing/non-array `choose`, invalid policy, or
animation kind rejects the pool. An empty pool is inert and can be selected,
but cannot be cycled. Pools cannot contain pools.

Pools are accepted by the four `[effects]` selectors, window rules'
`border_effect`/`window_effect`, output `screen_effect`, and matching runtime
actions. Animation bindings and border `overlay` remain preset-only.
Definitions and references may be in different included files. Inspection
keeps declaration order: included files before their including file, and source
order within each file. Members keep `choose` order.

Each mapped window owns independent border and window slots; each present
output owns a screen slot; the session owns one cursor slot. Holdings count
only unsuppressed owners assigned from that same pool. Hidden/scratchpad
windows, unfocused borders, disabled outputs, and failed/inert programs still
hold members. Plain presets, other pools, overlays, and closing snapshots do
not count.

Visibility and drawing gates do not allocate. A focus rule that changes the
winning selector resolves that selector. An unchanged selector retains its
member. On a pool change, a valid remembered member for the destination wins,
then the current member if it belongs to that pool, then a new policy pick.
Returning to a pool can therefore share a member with a newer window. Releasing
a holding never redistributes other owners.

## Runtime selection

The sixteen [effect actions](actions.md) operate independently on window,
border, screen, and cursor slots:

```sh
umbriel msg effect-window-set:scanlines
umbriel msg effect-border-cycle:borders
umbriel msg effect-window-toggle
umbriel msg effect-window-reset
umbriel msg effect-screen-set:vignette/HDMI-A-1
umbriel msg effect-cursor-set:glow
```

Window/border actions default to the focused window; screen actions use the
preferred output. Set and cycle accept a target after the first `/`, preserving
any further slashes in output names. An unnamed targeted cycle is
`effect-window-cycle:/<window-id>`. Toggle/reset take an optional target directly.
Cursor actions have no target. Explicit screen targets can address present
disabled outputs; their cached selection is visible when they are enabled again.

`set <name>` installs a runtime override, clears suppression, and makes a fresh
policy pick for a pool. `cycle [pool]` advances through that pool (or the
underlying current pool), installs an override, and clears suppression. If the
current member is absent from the requested pool, cycle uses its policy.
One-member pools cycle successfully; empty pools report `pool is empty`.

`set off` suppresses without replacing the selector or losing history; repeating
it does nothing further. Toggle flips suppression when a member exists and can
always unsuppress, even if a reload made the selection empty. Toggling an empty,
unsuppressed slot succeeds without changing it. Suppression releases the holding
and also hides a border's overlay. Rules/reloads continue updating the underlying
selection while suppressed. `reset` clears the override, suppression, history,
and cached assignment, then resolves configuration afresh.

Runtime overrides take precedence over rules and defaults. Invalid names,
kinds, targets, or payloads fail without changing selection or policy state.
A valid inert or failed program remains selected and renders plainly.

Unrelated reloads and renderer recovery retain assignments. Effects reloads
prune invalid history, retain still-valid members, and drop deleted/wrong-kind
runtime overrides with an owner-specific diagnostic, preserving suppression.
Unmap clears the window's state after copying closing visuals; remap starts
fresh. Output disable retains state, but disconnect/reconnect starts fresh.
Runtime state is not written to disk.

## Inspect selections

`umbriel effects` prints presets with program states, pools with exact hold
counts, the cursor selection, and window/output owners. `--json` provides the
same data for scripts. States are `inert`, `unreferenced`, `failed`, and
`compiled`. Only reached pools compile their members. Named keybinds and enabled
hot corners are prepared before their first trigger; an IPC-only name compiles
synchronously on first selection. Suppressed runtime overrides remain roots;
inactive pool history does not.

`umbriel windows --json` and `subscribe windows` include `border_effect` and
`window_effect` with `name`, `pool`, `source` (`default`, `rule`, or `runtime`),
and `suppressed`; borders also expose `overlay`. Inspection never picks,
compiles, or binds. Screen assignments are in `effects`, while `outputs --json`
keeps its existing output-management contract. See [IPC](ipc.md).

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
| `umbriel_size` | Drawn width and height in effect logical pixels, before overview zoom. |
| `umbriel_scale` | Buffer pixels per effect logical pixel, including overview zoom. |
| `umbriel_expand` | How far the drawn rectangle extends past the window on each side, as a fraction of its width and height. `(0, 0)` except for an animation running while drag physics deforms the window. |
| `umbriel_time` | Seconds on the animation clock, times the border's `speed`. Held as a single-precision float that is never wrapped, so fine time-based motion loses precision after long uptimes. `sin` and `cos` reduce their argument to one revolution, so they stay correct at large angles. |
| `umbriel_palette_count` | `4` for palette presets, `0` otherwise. |
| `umbriel_palette_at(float t)` | The palette color at `t`, blended between neighboring colors from the wrapping sequence `accent_primary`, `accent_secondary`, `warning`, `error`. Transparent black when there is no palette. |

Animations add `umbriel_progress`, `umbriel_clamped_progress`,
`umbriel_linear_progress`, `umbriel_direction`, and `umbriel_random_seed`
([Animation](animation.md#custom-effects)). Borders add `umbriel_border_hole`
(the client rectangle in `uv`), `umbriel_border_radius` (its corner radii in
logical pixels), and `umbriel_border_distance(vec2 uv)`, the signed distance
in logical pixels to the client rectangle, negative inside it; the client hole
is always cut out of a border's result. Cursor effects add `umbriel_pointer`,
the pointer position in `uv`. Motion-aware cursor shaders can read
`umbriel_pointer_count` (0–8) and `umbriel_pointer_history[8]`, oldest first.
Each `vec4` contains position in the same `uv` coordinates, age in seconds,
and a reserved component. Samples expire after 300 ms; older samples are
spaced approximately 32 ms apart while the newest tracks each motion event.
The drawn rectangle encloses the samples plus `radius` padding, so use
`umbriel_size` to calculate distances in logical pixels. History resets on
hide, output crossing, lock and program changes. Reading motion history
requests frames only while samples remain; reading `umbriel_time` still
requests continuous frames. No history buffers are allocated for this input.

For a longer tail, read `umbriel_pointer_path[64]` instead of
`umbriel_pointer_history`. This opts into a two-second history with up to 64
samples at the same approximate 32 ms spacing. `umbriel_pointer_count` reports
the active path length. The fourth component is a stable birth phase in
seconds (wrapping every 60 seconds), useful for retaining a sample's colour
and particle seed. Positions and ages have the same units as short history.
Long paths follow the same expiry, damage, frame gating and reset rules.

The bundled `trail` uses a fixed Noctalia-inspired palette, independent of the
desktop theme. Its colours can be edited in `cursor/trail/shader.glsl`. Both
`trail` and `trail-path` draw a Catmull-Rom curve through the samples, so fast
circles stay round; `trail-path` reads `umbriel_pointer_path` and is the
starting point for longer tails.

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

Referenced presets compile at startup, on effects reload, and during runtime action preparation. A compile error is logged with the
preset's name and the driver's message, whose line numbers count from the top
of the shader file; that preset renders plainly (opening and closing
animations keep their built-in animation, `style` and `scale` included) until
a reload fixes it. Unknown names, or a preset of the wrong kind for a
selector, report a diagnostic and are dropped: a top-level `[effects]`
selector selects nothing, and a window rule's or output's own override falls
back to an earlier matching rule or the `[effects]` default. Shaders are
trusted local GPU code; keep them small and side-effect free.

## Cost

Unreferenced presets and pools add no rendering work. Named keybinds and enabled
hot corners prepare programs before use; selections retain bookkeeping state.
A border effect renders the ring
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
