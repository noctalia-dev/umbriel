# Effects

Effects are the GLSL programs selected through `[effects]`, window rules,
output tables, and animation events. Configuration, the shader interface, and
user-visible behavior are in the [Effects guide](../user/effects.md); this
note records who owns effect state, where programs attach, how animation
presets drive lifecycle and motion slots, and which rendering costs an effect
may change.

## Ownership

### `EffectRegistry`

[`EffectRegistry`](../../src/scene/effect_registry.h) is a `Server` member and
the only place programs are compiled
([`effect_registry.cpp:181-257`](../../src/scene/effect_registry.cpp)).

- `prepare()` runs at startup (`server.cpp:335`), on a reload that sets the
  `animation` or `effects` flag (`server_events.cpp:562-564`), and after
  renderer recovery (`server_events.cpp:829`). No render callback compiles.
- Only referenced presets compile: a top-level selector, a window rule, an
  output's `screen_effect`, an enabled animation event's `effect`, or the
  `overlay` of a referenced border preset (`:121-151`). Entries are keyed by
  preset name and recompile only when the kind or source text changes;
  unreferenced entries are dropped. A failed compile logs one error and stays
  cached as null, so every lookup of that name renders plainly. A new renderer
  discards every program.
- The built-in fade compiles whenever `windows_in` or `windows_out` fades
  through it, independently of presets, and is the only program marked
  shape-preserving (`:204-214`). The drag deformation program compiles only
  here, and only while animations and `windows_drag.physics` are on
  (`:215-226`).
- `fillTimeUniforms` (`:323-343`) adds `umbriel_time` only to a program that
  reads it and the `[colors]` palette only to a preset with `palette = true`.
- `EffectLedger` ([`effect_ledger.h:12-58`](../../src/scene/effect_ledger.h))
  holds one `EffectInstanceState` per owner: the border node, the surface node
  for its window slot and that node's `addons` member for its overlay slot
  (`effects.cpp:151-152`), and the output and its cursor-owner member for its
  screen and cursor slots (`output.cpp:149`, `:160`). Each records its driving
  output,
  whether it is visible there, whether its program reads `umbriel_time`, and
  whether its clock advances. `eligible(output)` counts instances that are all
  three and is 0 while suspended; `active()` counts owners. `updateInstance`
  schedules an output's frame when its eligible count leaves zero
  (`effect_registry.cpp:278-288`).
- `syncLightLayer` (`:264-269`) keeps the light layer only while a compiled
  border preset defines `light`. `cursorEffectActive()` is true only while the
  default cursor preset compiled and the ledger is not suspended.

### `ViewEffects`

[`ViewEffects`](../../src/view/effects.h) is a `View` member.

- `resolve()` takes the resolved window rule's `border_effect` and
  `window_effect` over the `[effects]` defaults; `off` and `""` select nothing
  ([`effects_rules.cpp:6-17`](../../src/view/effects_rules.cpp)).
  `View::applyDynamicRules` calls it (`view.cpp:4797`), so rule and
  configuration changes re-resolve.
- `borderEffectApplies` (`effects_rules.cpp:23-25`) opens the border-effect
  slot only for a focused, decorated, non-urgent, non-fullscreen window. The
  border preset's `overlay` is bound only while that slot is, on the border's
  clock.
- `apply()` ([`effects.cpp:55-153`](../../src/view/effects.cpp)) binds the
  border-effect slot on the border tree and the window and overlay slots on the
  toplevel's surface tree node. The same window slots are bound in the view's
  isolated capture scene when `in_capture` is on and cleared there otherwise
  (`:122-130`).
- `View::syncAnimationEffects` (`view.cpp:1415-1510`) is the one entry point
  for the live view and its overview card: `Overview::syncCardEffects`
  (`overview.cpp:567-580`) passes the card's tree, border, surface tree, focus
  gate, and output. Its persistent block runs only when a preset
  is selected or the ledger has owners (`view.cpp:1482`).
- Unmap clears the window slots from both surface trees and removes the view's
  ledger instances (`view.cpp:3258-3264`, `:3291`).

### `Output`

`Output::applyOutputEffects`
([`output.cpp:117-169`](../../src/output/output.cpp)) sets the capture
policy, binds the output's screen preset (its `screen_effect`, else the
default) and the cursor preset, and records both in the ledger. It pushes the
pointer after setting a cursor program, because a new cursor program draws
nothing until a pointer update follows it. It binds no program while the
ledger is suspended, and returns before any scene call when no preset is
defined and the ledger is empty. The cursor square draws only on the output
holding the pointer: a pointer outside the output, or hidden, leaves that
output's cursor slot inactive (`wlr_scene.c:4199-4202`, `:4216-4218`). The
output also owns the effect frame timer ([Frames](#frames)).

### `Cursor`

`Cursor::forwardEffectPointer`
([`cursor.cpp:280-284`](../../src/input/cursor.cpp)) reads
`cursorEffectActive()` and forwards the pointer only when it is true. The
registry pushes the position to every output and re-applies output effects when
the output under the pointer or the pointer's visibility changes
(`effect_registry.cpp:265-276`). A move grab calls drag physics only when
`View::beginDragPhysics` accepted it (`MoveGrab::physics`).

## Attachment

Every node carries up to 13 slots
([`effect.h:18-39`](../../umbrielfx/include/umbrielfx/render/effect.h)).
Descendant slots compose before ancestor slots; on one node, slots compose in
index order. Slots 0-2 are persistent; the rest are transient, including drag.
The class follows the slot, not whether its program reads time.

| Index | Slot | Node | Class | Mode |
| --- | --- | --- | --- | --- |
| 0 | window | toplevel surface tree node, and its counterpart in the isolated capture scene | persistent | in place |
| 1 | overlay | as window | persistent | in place |
| 2 | border effect | view border tree | persistent | capture, `expand` |
| 3 | border | view border tree | transient | capture |
| 4 | dim unfocused | view content tree | transient | capture |
| 5 | windows_move | view content tree | transient | capture |
| 6 | drag | view content tree | transient | capture, `expand` |
| 7 | windows_in | view content tree | transient | capture |
| 8 | windows_out | close snapshot root | transient | capture |
| 9 | scratchpad | view content tree and backdrop targets | transient | capture |
| 10 | layers | layer tree or its close snapshot | transient | capture |
| 11 | workspaces | output workspace view root | transient | capture |
| 12 | overview | per-output overview tree | transient | capture |

An overview card carries the view's slots, except drag, on its tree, border,
and surface tree, which holds every mirrored surface buffer as the view's
surface tree holds its surfaces.

`wlr_scene_node_copy_animations_for_snapshot`
([`wlr_scene.c:1776-1795`](../../umbrielfx/types/scene/wlr_scene.c)) copies
each populated slot with its current parameters, moves its feedback history,
and turns light off. Slots from `windows_out` up land in `windows_in`; the rest
keep their index. Nothing updates the copied slots' parameters, so their time
stays frozen, and a snapshot owns no ledger instance, so it never requests
frames.

| Source | Snapshot node | Slots |
| --- | --- | --- |
| View content tree | snapshot root (`view.cpp:2480`) | content-tree slots, then `windows_move` is cleared |
| View surface tree node | snapshot content tree (`view.cpp:2483-2487`) | window, overlay |
| View border tree | each copied border (`border_rect.cpp:39-41`) | border effect, border |
| Card tree, surface tree, border | snapshot root, copied surface tree, copied border (`overview.cpp:1116-1143`) | as the live card |
| Layer tree | snapshot root (`layer_surface.cpp:197`) | layers |

Drag physics binds the built-in deformation program to the content tree's drag
slot with `umbriel_deformation[16]` and an `expand` of the sheet's
displacement bound plus 2 px (`view.cpp:1453-1473`, `:1137`). The sheet spans
the content tree's drawn bounds from `wlr_scene_node_effect_bounds` and is
refit on every tick (`view.cpp:1091-1117`); a re-grab while it settles keeps
the sheet and its transition. `View::animatesOn` includes every output the
drawn box reaches (`view.cpp:1624-1628`). The program is not shape-preserving,
so `render_animation_shadow` (`wlr_scene.c:3472-3542`) captures the content
tree and the drop shadow follows the deformation within the shadow node's own
region. A close mid-drag moves the drag slot to the snapshot root, and
`CloseSnapshot::applyShrink` grows its tree clip by that slot's `expand`
(`server.cpp:1134-1136`), so `popin` and `zoom` keep the frozen deformation.

A dragged window sits in the unclipped drag tree
(`View::enterDragPresentation`), so every output it reaches draws its part.
When its drawn bounds cross an output edge, a group-sized capture retains the
off-output input before the drag slot deforms it. A border outside the monitor
can bend back onto it; only the finished group is clipped to output damage.
A physical-pixel translation preserves fractional rounding and rotation.
Enclosing backdrop layers use the same translation. The group capture and
everything nested in it allocate from a separate offscreen set
(`fx_offscreen_buffers.group`), so output-sized captures elsewhere in the frame
never resize them; a pass without a group capture drops that set. Capture
dimensions round up to 128-pixel blocks to reuse buffers as the spring's
margin changes.
`drag/physics_edges` covers all four edges and nested effects on a
rotated, fractionally scaled output.

## Slot modes

**Capture.** `render_animated_range` (`wlr_scene.c:3715-3869`) renders the
node's contiguous descendants into an offscreen buffer per capture slot, runs
their own effects first, then composites through each program over the node
bounds grown by the largest `expand` among the node's border-effect and drag
slots (`fx_slot_expands`). A border-effect composite receives the border's hole
and radii (`scene_border_geometry`, `:3547-3578`), and the preamble cuts the
hole out of its result. A persistent capture slot runs only when this frame's
damage reaches its drawn box (`:3763-3773`); otherwise nothing composites and
its history carries over.

**In place.** After the subtree is drawn, `render_in_place_slots`
(`:3632-3700`) calls `fx_render_pass_effect_in_place`
([`fx_pass.c:1243-1283`](../../umbrielfx/render/fx_renderer/fx_pass.c)),
which copies the current target under the node's rectangle into the output's
`in_place_source` offscreen buffer and runs the program with that copy as
`umbriel_sample`. The result is written back unblended through `umbriel_mask`,
which rounds the rectangle with the corner radii of the node's first surface
buffer. When that buffer has a corner box (content inside client-side margins),
the rectangle is the corner box within the node bounds, with square corners
wherever the bounds cut it; without one, the rectangle is the node bounds,
margins included. A failed copy skips the write-back and logs once. In-place
slots never run in the unfiltered capture composition ([Capture](#capture)).

**Window sampling contract.** An in-place slot reads the current target. At
rest, with no enclosing capture, that is the output framebuffer after the
window is drawn: the program sees, and may rewrite, the desktop visible through
a translucent window. Inside an enclosing capture (drag, open, close, move,
workspace, overview), it is the capture buffer: the program shades the
window's own content, and the result is composited over the live desktop. The
desktop beneath is not captured and not moved with the window. The overlay
slot follows the same contract.

## Border light

- **Layer.** `Server::setEffectLightLayer`
  ([`server.cpp:927-938`](../../src/server/server.cpp)) creates a scene-root
  tree directly above the drag-icon tree and below the top shell layer, and
  registers it with `wlr_scene_set_effect_light_layer`.
- **Proxy.** `scene_light_sync` (`wlr_scene.c:1234-1295`) keeps one
  input-transparent rect per lit border slot in that layer, covering the
  border's bounds grown by `ceil(2 × spread + 8)` logical px plus the slot's
  `expand`. The rect carries visibility, output membership, and damage; the
  renderer draws the light in its place (`:3024-3039`).
- **Emission.** Each display composite of the slot runs `emit_light`
  (`fx_pass.c:984-1053`): the program again, unblended and without a history
  write, into a full-resolution emission texture (half float when the renderer
  can filter it); a threshold pass into level 0 of a half-resolution pyramid
  with the proxy's margin; then Kawase down and up passes over 1 to 6 levels
  chosen from the spread. `fx_render_pass_add_effect_light` (`:1055-1079`)
  screen-blends level 0 over the proxy's box at `intensity`. The pyramid
  belongs to the slot and holds its latest composite; every output showing the
  proxy draws it. The helper program compiles once per renderer, on first use.
- **Suppression.** No proxy exists while the layer is absent, `intensity` or
  `spread` is 0, the border node is disabled, or its root-level ancestor sits
  above the light layer (fullscreen and pinned windows). Under a transient
  ancestor the proxy stays, disabled. Snapshots copy the slot with light off.
  Shadow captures and the unfiltered capture composition do not emit.

## Frames

An instance is eligible while it is visible on its output
(`wlr_scene_node_visible_in_box` against the output's layout box; the output
enabled for a screen slot; the pointer shown on that output for a cursor slot),
its program reads `umbriel_time`, and its clock advances (for a border or
overlay, `animated` and a nonzero `speed`; for all, an unfrozen animation
clock).

`Output::handleFrame` (`output.cpp:1228-1252`) treats a frame as an effect
frame when one was requested, or when an instance there is eligible and the
`max_fps` interval has elapsed (`effectFrameDelayMs`,
[`frame_schedule.h:35-44`](../../src/output/frame_schedule.h); 0 follows the
refresh rate), whatever scheduled the frame. The output's effect time,
`Output::effectSeconds()`, is stamped from the animation clock only on effect
frames, and while nothing on the output is eligible but a persistent preset is
referenced or the ledger has owners (`output.cpp:1241-1242`), so it advances
at most at `max_fps` and a new instance starts from the present. After the frame,
`Output::armEffectFrame` (`:1110-1127`) requests the next frame at once or arms
a lazily created timer for the rest of the interval; with nothing eligible, or
while the session is locked, the timer is disarmed (`:1462-1466`).

Locking suspends the ledger and re-applies output effects, which unbinds
screen and cursor slots (`Server::activateSessionLock`,
`server_events.cpp:1402-1403`); unlocking resumes it and reschedules outputs
with eligible instances (`:1425-1431`). Border and window slots stay bound, and
the lock surface carries no slot.

A persistent effect never finishes, so it stays out of the animation registry.
`Server::settled()` (`server.cpp:1391-1419`), `Server::animationsActiveFor`,
and the render lock and tearing veto that follows it (`output.cpp:1262-1268`,
`:1308-1309`) see only transient animations: a time-reading effect never
blocks `settle` or holds `wlr_output_lock_attach_render`. The drag sheet ends,
so it is an animation. The `effect-frames` IPC
([Harness-only IPC](README.md#harness-only-ipc)) reports each output's effect
frames and eligible count.

## Capture

`Output::effectCapturePending` (`output.cpp:191-194`) is true while a capture
holds a render lock on the output, `in_capture` is false, and a referenced
in-place preset (window, overlay, screen, or cursor) compiled. It is keyed on
configuration because a close snapshot keeps window slots without a ledger
instance. Capture locks are the external attach-render locks minus the
animation lock and minus export-dmabuf frames on that output (`:177-189`), so
an export-dmabuf client reads the displayed frame.

With a capture pending and an in-place slot or output effect visible on the
output, `wlr_scene_output_build_state` composes twice (`wlr_scene.c:5299-5328`,
`:5643-5660`). The unfiltered composition skips in-place slots, output
effects, and light emission, runs capture composites (the border effect and
every transient slot) with capture-role histories, and draws the software
cursor; `fx_render_pass_save_effect_capture` (`fx_pass.c:2836-2870`) then
copies the target into the output buffer's effect capture. The display
composition then starts again from the background. This pass damages the whole
output. When the save fails, it logs once, and the unfiltered composition
serves display and captures alike for that frame, with no output effects
(`wlr_scene.c:5682-5685`). `fx_texture_from_dmabuf`
([`fx_texture.c:532-554`](../../umbrielfx/render/fx_renderer/fx_texture.c))
substitutes a valid effect capture for any import of that output buffer, which
is how screencopy and image-copy receive the unfiltered frame.

Feedback history
([`animation_history.h`](../../umbrielfx/internal/render/fx_renderer/animation_history.h))
is kept per animated node's slot, per output, per renderer, and per composition
role: 0 for display, 1 for the unfiltered capture (`wlr_scene.c:3854`). Each
entry holds two buffers, allocated only for programs that call
`umbriel_sample_previous`. A pass reads and promotes only its own role's entry;
a first frame, or a missing entry, reads that pass's current input. Promotion
waits for a successful submission (`fx_pass.c:288-298`), at most once per
frame; shadow captures read history but never promote it. A new transition or
program resets every role; a renderer, output transform, or format change
drops the affected entry's buffers. When a capture ends or the capture policy
changes, the output's capture-role entries and effect captures are released
(`wlr_scene.c:4075-4098`, `:5330-5334`), and
`Output::scheduleEffectCaptureRelease` draws one more frame so that happens
promptly.

Each view's isolated toplevel capture renders its own `wlr_scene`
(`view.cpp:212-216`), so its slots, histories, and policy counts never touch
the desktop scene. Window slots are bound there only with `in_capture = true`.

## State, scanout, damage, and culling

Each scene keeps a `scene_effects` addon on its root (`wlr_scene.c:151-206`)
listing its `scene_animation` addons with separate counts of nodes carrying
transient and persistent slots (`scene_animation_classify`, `:208-223`). The
addon exists while any node carries a slot or a light layer is registered, and
is destroyed with the last of them (`:359-364`, `:1313-1350`). An
output's `scene_output_effects` addon (`:3990-4005`) is created on first use:
a screen or cursor slot, `in_capture = true`, or an unfiltered composition.
`wlr_scene_output_build_state` looks the scene addon up once per frame. A
transient slot anywhere keeps the scene-wide conservative policy on every
output; persistent slots never contribute to it.

`render_data.persistent_visible` (`:5301-5318`) is true when a render-list
entry sits under a node with a persistent slot, or the output has a screen
effect or a shown cursor effect.

| Site | Transient slot in the scene | Persistent effect |
| --- | --- | --- |
| `scene_node_opaque_region` (`:684-769`) | No node is opaque. | A node at or under a node with slots contributes no opaque region; every other node keeps its own. |
| `scene_entry_try_direct_scanout` (`:4694-4705`) | Veto on every output. | Veto only where `persistent_visible`. |
| Animation-buffer release (`:5319-5321`) | Buffers kept. | Kept only where `persistent_visible`; released elsewhere. |
| `calculate_visibility` | Render-list culling off (`:5260`). | Culling stays on. The update pass keeps an occluded node under a persistent effect visible, so it keeps output membership and frame callbacks (`:1097-1105`); an entry whose visible region, grown by the effect's `expand`, reaches the output is kept (`:4582-4599`); the background-color skip exempts nodes under an effect (`:4560`, `:4572`). |
| Whole-output damage (`:5342-5344`) | Every frame. | Never from presence alone. `expand_damage_to_effects` (`:5122-5162`) grows commit (`:5382`) and render (`:5499`) damage to every effect box it touches, until nothing grows, because a program may read any texel of its box. Only the unfiltered capture pass damages the whole output (`:5337-5340`). |
| `fx_render_pass_init_offscreen_buffers` (`:5582-5591`) | Always. | Only where `persistent_visible`. |

A drawn box is the node's bounds grown by its `expand`, plus the light proxy
(`persistent_effect_box`, `:5069-5099`); a screen or cursor box is the output
or the cursor square. The render-list walk tests leaves against the output box
grown by the scene's largest `expand` (`scene_effects_max_expand`), so a node
whose drawn box reaches an output only through its margin is listed there and
its program runs over that margin. What the program samples is still that
output's capture, so a drag sheet shows only what that output captured
([Attachment](#attachment)).
Changing a slot (`wlr_scene_node_set_animation`,
`:1637-1734`) updates the whole scene for a transient slot. For a persistent
slot it damages the drawn box before and after the change
(`scene_effect_damage`, `:1546-1567`) and re-runs `scene_node_update` on the
node when a slot appears or disappears. Destroying a node damages its effects'
margins first (`:1587-1595`). Whenever the scene has effect state,
`scene_node_update` (`:1484-1541`) grows its update and damage regions by the
largest `expand` on the node, its ancestors, and its enabled descendants
(`scene_node_drawn_expand`, `:1457-1463`), so moving a frame repaints a child
slot's old margin.

## Cost

With no effect selected and drag physics off:

- `prepare()` compiles nothing but the built-in fade, which the lifecycle
  settings alone decide; no deformation program, light layer, output-effect
  addon, effect timer, or ledger instance exists.
- Per view sync: `ViewEffects::configured()` and the ledger size
  (`view.cpp:1482`). Per output frame: an eligible count over the empty ledger;
  the clock is never read for effect time; `expand_damage_to_effects` returns
  at once; `Output::effectCapturePending` is false, so no second composition
  runs. Per pointer motion: one boolean (`cursor.cpp:281`).
- Drag physics: `Cursor` gates every call on `MoveGrab::physics`, and
  `View::tickAnimations`, `View::hasActiveAnimations`,
  `View::syncAnimationEffects` (`view.cpp:1453`), and `View::animatesOn`
  (`view.cpp:1626`) read `DragPhysics::active()`/`grabbed()` per view per tick
  and sync, with no writes, scene calls, or allocations.
- Scanout, damage, and culling take only the transient branches in the table
  above, as they do for built-in animations. Reload prepares only with the
  `animation` or `effects` flag; renderer recovery prepares the built-in fade.
- While any slot exists, built-in animations included, `scene_node_update`
  walks the updated node's ancestors and descendants for their largest
  `expand`.

With effects selected:

- **Border:** a capture and one program pass per frame on the focused window;
  effect-only frames for a time-reading program, capped by `max_fps`. Light adds
  a second program evaluation into a full-resolution emission texture, a
  half-resolution pyramid, and its blur on each display composite, and a blend
  on outputs showing the proxy.
- **Window and overlay:** a copy of the target under the window and one program
  pass per window per frame.
- **Screen and cursor:** one in-place pass over the output or the cursor square;
  direct scanout off on that output only.
- **Drag physics:** only while a window is held or settling; the drag slot is
  transient, so the scene-wide policy applies on every output for that time.
  The deformation program runs up to 22 inverse-lookup iterations per pixel.
- **Persistent border, window, and output effects:** damage within drawn boxes,
  culling exceptions only for affected nodes, and the scanout veto and
  offscreen buffers only on outputs where the result is visible.
- **`in_capture = false`:** a second composition on frames with a pending
  capture and a visible in-place slot or output effect, with whole-output
  damage, plus separate capture-role history buffers for programs that sample
  previous results.

## Animation slots

An `animation` preset can drive every animation event except
`windows_drag`, whose slot runs the built-in deformation program. Configuration
and authoring details are in [Animation](../user/animation.md#custom-effects).

### Configuration and compilation

Animation events bind a preset by name: `[animation.<event>] effect = "<name>"` must name an
`[effects.preset.<name>]` with `kind = "animation"`. `readShaderSource` (`src/config/effects.cpp`) reads
the preset's `shader` path: inline GLSL is not accepted, paths resolve relative to the declaring TOML
file including included files, missing files stay watched, contents take part in configuration
equality, and blank/NUL text, nonregular files, and inputs larger than 256 KiB are rejected.
Nonblocking opens prevent FIFOs hanging config reload.

`EffectRegistry` (`src/scene/effect_registry.cpp`) caches one program per referenced preset, kind, exact
source, and renderer. Startup and any reload that changes `[animation]` or `[effects]` prepare programs
before rendering. A reload while a transition is running keeps that transition's program but rebinds
its uniforms from the new configuration, so a program reading `umbriel_time` or the palette can see
them reset for the rest of that transition. Failures are cached too, avoiding per-frame compiler
retries. UmbrielFX supplies a GLSL
ES 1.00 wrapper around `vec4 animation(vec2 uv)`, normalized target sampling,
target-local previous-result sampling, a stable four-channel random seed,
logical target size, eased and linear progress, and transition direction.
Compiler diagnostics retain source line numbers and the label: the shader file path, or
`effects.preset.<name>` when the preset has no file.

Without an effect, `windows_in` and `windows_out` bind a built-in fade
program through `EffectRegistry::lifecycleEffect`, except for the `slide` style, whose opacity
curve differs from its progress. The window, its subsurfaces, and its border
are composited once and faded as a group, so overlapping surfaces never show
through each other mid-fade. `View::fadeComposited` and the close snapshot keep
buffer and border opacity at 1 while such a program runs. The built-in program
is marked shape-preserving, so its window keeps the analytic shadow, which the
view fades itself. Scratchpad and layer fades stay per buffer, because they can
start from a partial alpha that normalized progress does not carry.

### Scene processing

Effect state is attached through scene-node addons, preserving the scene ABI.
Thirteen ordered slots permit simultaneous effects on a node. Descendant
effects run before ancestor effects; same-node order is window, overlay, border
effect, border, dimming, movement, drag, opening, closing, scratchpad, layers,
workspaces, then overview. Slots 0-2 are persistent effects
([Effects](effects.md)); the rest are animation events.

| Event | Target and timeline owner |
| --- | --- |
| `windows_in` | View tree and existing map fade |
| `windows_out` | Close snapshot, including a card closed from overview, and existing close fade |
| `windows_move` | View tree and position/presentation-size animation |
| `windows_drag` | View content tree's drag slot and the `DragPhysics` sheet |
| `workspaces` | Output workspace view root and workspace slide |
| `overview` | Per-output overview tree when entering or leaving overview and zoom/row settling |
| `scratchpad` | View show/hide fade and separate dim/blur backdrop targets |
| `border` | Border tree and focus-color animation |
| `dim_unfocused` | View tree and focus-opacity animation |
| `layers` | Layer tree or close snapshot and map/unmap fade |

The renderer captures contiguous descendants from the scene's paint-ordered
render list into an alpha framebuffer, recursively processes inner effects,
then runs each enclosing shader once. Subsurfaces therefore share the window's
effect rather than restarting it. Overview cards reuse the source view's
animation state. Window shadows sit beside the animated content tree, never inside it.

An animation node can carry a final-composite output clip in node-local
coordinates. Capture, shader input, and feedback history retain the full target;
only the final active slot is intersected with this clip. An empty clip therefore
keeps custom shader evaluation and history advancement alive without
compositing pixels. If no shader composite exists, the caller uses an ordinary
scene-tree clip instead. Shadow silhouette capture ignores the source output
clip, then the separately stacked shadow receives the corresponding visible
clip.

Shadow nodes hold an addon association with their source window, without
changing the scene ABI. While that window or a descendant has an active shader,
the renderer captures its post-effect alpha separately from the backdrop and
other windows. Two separable Gaussian passes produce a colored, offset shadow
at the shadow node's original stacking position. The unblurred silhouette
excludes visible content, so translucent windows are not tinted by their own
shadow. Source alpha supplies opacity once; the configured shadow color is
not preattenuated by the native fade. Ancestor shaders are excluded from the
caster capture and subsequently process the window and shadow together.

The normal analytic rounded-rectangle shadow remains the fast path without
window-subtree shaders other than shape-preserving ones, and the fallback on capture or internal-program failure.
The two internal programs are cached per renderer, including failures. Shadow
captures use the existing output/depth buffer pool and working color format.
The horizontal pass uses a reduced grid matched to the kernel spacing; linear
reconstruction prevents visible tap bands at large softness without increasing
the tap count. FP16 targets without hardware linear filtering use shader-side
bilinear reconstruction.
They do not emit duplicate surface sampling notifications or include backdrop
blur. Blur sampling clamps at output edges to avoid artificial shadow seams
along the output clip. Shadow offsets rotate and scale only at rasterization.

The compositor remains authoritative for geometry, input, clipping, focus,
surface configuration, and lifetime. Lifecycle shaders replace native window
fade/scale/slide visuals; other events postprocess their native presentation.
Logical target bounds are converted to output pixels only at rasterization.
Sampling transforms account for output rotation and fractional scale, with
transparent samples outside target/output bounds. Drawing honors ancestor clips.

Shaders that call `umbriel_sample_previous` receive the prior successfully
submitted post-shader result for the same node, slot, output, renderer, and
composition role ([Capture](#capture)). The first render uses the
current input. History uses normalized target coordinates, so it follows
movement and is resampled across target-size changes. A transition ID
distinguishes retargets from spring progress moving backward. Snapshot creation
preserves that ID and seed and transfers feedback history before the source
target is retired.

Previous-result feedback lazily allocates two target-sized buffers per active
node, slot, output, and composition role. The pair costs 8 bytes per pixel in
SDR and 16 bytes per pixel with an FP16 color-management target, before
allocator overhead. History is reset for a new transition, program, output
transform, working format, or renderer. Allocation or import failure disables
feedback for that transition and keeps direct shader rendering available.
Front-buffer promotion is deferred until render-pass submission succeeds.
Shadow silhouette captures may read the same prior result but never advance it.

Intermediate buffers are pooled per output and nesting depth, allocated on
demand and dropped after effects end. Composition preserves the working format,
including FP16 when color management uses a linear intermediate. Blur within
an effect samples a reconstructed backdrop containing outer captures and earlier
siblings. Texture imports are checked before capture; allocation/import failure
leaves ordinary rendering available. Capture depth is bounded at 24.

While any node in a scene carries an animation slot, opaque-region culling and
direct scanout are disabled and every output receives full damage. This
conservative policy allows arbitrary target sampling and changes in alpha
without stale pixels. Persistent effects confine these costs to the nodes and
outputs they affect ([Effects](effects.md#state-scanout-damage-and-culling)).

### Lifetime

Nodes and the compilation cache hold independent program references. In-flight
transitions retain their program across source edits until completion or
retargeting. Removing/disabling an effect clears its active slot. Close
snapshots copy current window and border effect parameters, retaining an
interrupted opening effect inside the new closing effect. Layer unmap capture runs before the
scene helper disables its subtree.

Window close snapshots own a separate shadow tree: directly below the snapshot for
a window that casts its own shadow, and below every window of the output for a tile.
It follows the snapshot position, retains the source shadow settings, and is
destroyed with the snapshot. Its source association is detached safely when
either node is destroyed. Analytic fallback shadows follow the native lifecycle
fade even when a custom shader replaces the window's own fade.

A freshly admitted tiled opener never joins the reflow it caused, but it runs
alongside it. `View::handleMap` holds the opener with `View::deferTiledOpening`,
which disables its node and resets its fade, only until the admitting arrange
places it. `Workspace::applyTiledMotion` then calls `View::resumeTiledOpening`
and presents the opener at its final slot in the same pass that starts
`windows_move` for the established members, so both clocks begin on the same
tick. The opener's box is never interpolated: popin and zoom scale its
presentation around the final slot, and it is raised above the members that
reflow beneath it. An overview card mirrors buffers rather than the live node,
so `Overview::layoutCard` reads `View::tiledOpeningDeferred` and hides the card
until the same arrange.
A tile still running `windows_in` becomes an established geometry participant
when a later tile is admitted. Its cached unscaled layout box joins
`windows_move`, while `windows_in` remains composed over each presentation.
This separation also prevents popin and zoom from applying their scale to an
already scaled box.

A window that opens fullscreen rests in the output box its workspace assigns,
which the arrange that follows its map owns. `popin` and `zoom` therefore do not
tween the node: `View::fullscreenOpeningActive` keeps that box authoritative
while the fade centers a scaled presentation inside it on every tick, and the
layout paths record the resting origin instead of animating toward it. The
fullscreen backdrop is the window's own letterbox and follows the presented box,
so the opener scales with its surround rather than inside an output-wide one.

A normal tiled close snapshots the complete decorated view, drops any copied
movement effect, and is appended above every workspace tree under the output's
view root. It is a fixed canvas: `CloseSnapshot::present(canvasX, canvasY,
visible)` only translates it with its workspace's slide and hides it while that
workspace is not showing. `umbriel_size` is therefore constant for a close
snapshot unless the built-in `popin` or `zoom` style shrinks it, which scales
the frozen buffers, borders, and shadow toward the captured box's center using
`1 - alpha`, the same eased progress its fade follows.

The snapshot's `windows_out` `AnimatedValue` owns eased progress, linear
progress, transition identity, random seed, feedback history, and lifecycle
duration. Nothing outside it retimes a close.

The arrange pass sends each survivor's target-size configure and begins the
configured `windows_move` in the same pass, with no alignment delay and no
cached client commits. Both clocks run from the close, each with its own
duration and curve. With movement disabled, live geometry snaps while the close
lifecycle continues independently.

A layout-sized view that redraws at its requested size crossfades into it. On
the root surface's `client_commit`, while the scene still shows the outgoing
state, `View::handleClientCommit` checks that the commit acks the tiled size
request and changes the buffer size. If so, `ResizeCrossfade` clones the
toplevel surface tree's buffers into a hidden tree directly above it. The
applied commit then shows that frame and fades it out on its own `windows_move`
clock, over the live buffers. Both layers are scaled into the same presented
box on every presentation change, round against the same content box, and
multiply the view's opacity. The clones reject input and live inside the view
tree, so movement, clipping, and view shaders apply to them. Unmap, destroy,
and a canceled size animation discard the capture.

When movement completes, `completeLayoutMotion` keeps the compositor-owned
endpoint until both the configure serial and committed content dimensions
match the tiled size request. Geometry-stable arrange passes preserve that
hold. This prevents a late or size-refusing client from replacing the final
presentation with an older buffer size.

A close that interrupts consume, expel, or another layout motion rebuilds the
survivor set from the exact current presentations and starts a fresh
full-duration `windows_move`. A motion already heading for the same targets
keeps its running clock instead of restarting. Every close snapshot keeps its
original `windows_out` clock. Durations are not summed, and an earlier close
shader is not restarted or starved.

Logical tiled geometry uses `MonotonicEasing`. Curves already bounded and
monotonic keep their exact easing. Overshooting or reversing curves are
projected onto cumulative travel from zero to one across the full configured
duration. Geometry therefore never reverses, crosses a protected boundary, or
pins at the first endpoint crossing. The `windows_move` shader slot remains
bound to the original `AnimatedValue`, so `umbriel_progress` retains the raw
eased curve and `umbriel_linear_progress` retains its raw linear clock.

Every ordinary view close snapshot is tracked by its source workspace,
including snapshots that do not participate in tiled layout geometry. The
workspace translates their captured geometry during a workspace slide and
applies an empty final mask while the workspace is neither active nor
transitioning. Workspace destruction also empties the mask while leaving
server-owned animation teardown to the normal post-tick reap.

Closing a card from overview remains an independent `windows_out` lifecycle. It
does not join workspace vacancy coordination or alter motion owned by the
`overview` event.

Scene destruction releases addon references. Renderer destruction invalidates
remaining programs without accessing a dead context; renderer replacement
prepares new configured programs. Timeline owners clear finished effects and
continue to own cancellation, unmap, and teardown. No shader extends a timeline.
Each owner creates a process-unique nonzero transition ID and four pseudorandom
seed channels when it retargets. The ID and seed remain stable through ticks,
coordinate translation, spring reversals, and snapshots.

Custom GLSL is trusted local GPU code. Source-size and resource bounds do not
sandbox shader execution or prevent an expensive shader from stalling a driver.

### Regression coverage

`tests/unit/shader_source.cpp` exercises source validation and file reads.
`tests/unit/config_load.cpp` covers all event sections, included-file provenance,
source-content reload effects, dependency deduplication, and dependency removal.
`tests/unit/animation.cpp` verifies that tiled geometry preserves safe curves and
projects overshooting or reversing curves into bounded monotonic full-duration
travel. `tests/unit/layout_motion.cpp` covers shared-progress interpolation and
the separation test that orders a rearrangement's raises.

The isolated running-compositor checks `effect/animation`,
`effect/animation_events`, `effect/animation_transform`,
`animation/tiled_close_lifetime`, `animation/tiled_open_reflow_timing`,
`animation/tiled_close_no_reflow`, `effect/animation_tiled_open`,
`animation/tiled_lifecycle_move_timing`, `animation/close_snapshot_workspace`,
`animation/tiled_close_retained_no_reflow`, `effect/animation_tiled_close`,
`animation/tiled_close_configure_barrier`, `animation/tiled_close_fixed_snapshot`,
`animation/tiled_repeated_close`, `animation/dwindle_many_close_timing`, and
`overview/card_lifecycle` inspect
shader-specific intermediate pixels, file-watcher reloads, every animation
event, both layer lifecycle directions, rotated fractional-scale UVs, nested
sampling, output containment, invalid-GLSL fallback, and a tiled close effect
that outlasts its configured `windows_move` timeline. `animation/tiled_open_reflow_timing` and `effect/animation_tiled_open` also
verify that a tiled opener runs its own `windows_in` in its final slot on the
same tick its established neighbors begin to reflow, with each clock keeping
its own duration.
`animation/close_snapshot_workspace` covers tiled and non-layout close snapshots following workspace
translation and visibility while their independent lifecycle continues.
`animation/tiled_close_retained_no_reflow` keeps a no-reflow close fixed through an unrelated opening and
consume. `effect/animation_tiled_close` verifies that ordinary, consume, expel, and
disabled-movement closes preserve natural early, middle, and late shader phases
while survivors start moving at once. `animation/tiled_close_configure_barrier` verifies immediate target
configures and immediate visible movement, plus compositor endpoint ownership
for a client that retains stale buffer dimensions across a later stable
arrange. `animation/tiled_close_fixed_snapshot` covers longer, equal, and shorter `windows_out` duration
orderings against `windows_move` and asserts that the snapshot holds its
captured box for every sampled frame. `animation/tiled_repeated_close` interrupts scrolling reflow
with a second close and verifies independent shader phases, monotonic survivor
motion on a single movement clock, and no summed delay or starvation. `animation/builtin_window_styles` fades a window whose opaque subsurface covers its parent and verifies
that the parent never shows through while opening or closing. `animation/tiled_resize_crossfade` redraws a reflowing neighbor in a new color at its new size and
observes blended pixels mid-reflow, then the redrawn frame alone. `animation/dwindle_many_close_timing` closes
the root leaf of a five-window dwindle tree and verifies that every changing
survivor follows an overshooting movement curve across its full configured
clock instead of pinning at its first endpoint crossing. The overview check
verifies that a card close remains outside workspace vacancy coordination and
that the closing card drops its copied movement effect before `windows_out`
samples the captured client buffer. The `effect/animation_lifetime` and
`effect/animation_squash` checks load the bundled
`examples/effects/animation/{reveal,squash}` sources and also cover
program retention across reloads, close-during-open snapshots, shader removal,
and the bundled squash effect's intermediate pixels. Bright-green shadow
assertions in `effect/animation_squash`, `effect/animation_shadow`,
`effect/animation_shadow_lifetime`, `effect/animation_shadow_composition`, and
`effect/animation_shadow_close` cover
shader-created edges, normal-shadow restoration, closing teardown, rotated
fractional-scale offsets, reload and close-during-open retention, stacking,
translucent interior exclusion, enclosing workspace effects, descendant-only
border effects, retained closing borders, and smooth blur gradients. Negative controls
temporarily bypass shader rendering and configuration assignment to ensure
the checks fail for the behavior they cover. Shadow negative controls bypass
silhouette rendering in a separate temporary build, never in the working source.

`effect/animation_feedback` verifies first-frame initialization, recurrence,
target isolation, and feedback transfer into a closing snapshot.
`effect/animation_seed` verifies all four channels, stability within a
transition and its closing snapshot, and renewal when the same client maps
again. Unit coverage verifies global transition identities and preservation
while underdamped spring progress reverses. Negative controls replace feedback
with current-target sampling, force a reused seed, suppress transition renewal,
and refresh spring identity on every tick. Each check fails on its intended
behavior.

`render/renderer_recovery` verifies that the built-in fade compiles once per
renderer and that an animation preset rebinds to the replacement renderer's
program.

Headless checks do not establish physical HDR output correctness or hardware
GPU-reset recovery. Those require suitable hardware and a running-session check.

## Regression coverage

Unit ([`tests/unit`](../../tests/unit)):

- `effects.cpp`: kind names, reference validation and `off`, name resolution
  and border gating as pure functions, padding, screen overrides, clock
  precision, and ledger eligibility for advancing and frozen instances,
  visibility, suspension, and output removal.
- `drag_physics.cpp`: the pinned grab, trailing motion, bounded displacement
  and velocity, no folding, contraction under resize, settling after release
  and while held, frame-rate independence, and re-grab continuity.
- `border_ring.cpp`: padding grows the ring box around the same hole.
- `config_load.cpp`: per-kind keys, missing kinds and inert presets, selectors
  at every level, deferred reference validation, palette order, duplicate
  presets across includes, `animation.<event>.shader` as an unknown key,
  `windows_drag.physics`, the reload flag, and the bundled presets defining
  without selecting.
- `config_change.cpp` and `output_frame_schedule.cpp`: which edits raise the
  `effects` flag, and the `max_fps` delay.

umbrielfx (`meson test --suite umbrielfx`, cases `effects-*` from
[`umbrielfx/tests/effects.c`](../../umbrielfx/tests/effects.c)):

- Programs: `kinds`, `reads`, `uniforms`, `expand` (with its feedback history),
  `renderer-destroy`.
- Scene policy: `persistent-scene`, `occlusion`, `transient-policy`,
  `visible-in-box`, `effect-bounds`.
- Damage: `damage-confinement`, `whole-box-invalidation`, `damage-expansion`,
  `margin-damage`, `move-margin-damage`.
- Borders: `border-geometry`, `border-geometry-tree`, `border-light`,
  `border-light-lifecycle`.
- In place: `in-place`, `in-place-feedback`, `in-place-shape`.
- Capture: `capture-policy`, `capture-policy-encoding`, `capture-feedback`,
  `capture-composition`.
- Output effects: `output-effects`.

Harness ([`tests/harness/checks`](../../tests/harness/checks)):

| Check | Asserts |
| --- | --- |
| `effect/registry` | Only enabled references compile, compiled and failed programs survive unrelated reloads, a repaired source recompiles, and the bundled presets compile only once selected, each without diagnostics. |
| `effect/border` | Padding, the client hole, focus following, per-window `off`, the frozen clock, light over a neighbor, frames only while the clock advances, and the card's close snapshot. |
| `effect/border_transform` | Logical `uv` and `umbriel_border_distance` on a rotated, fractionally scaled output. |
| `effect/border_frames` | Effect frames follow the animation clock, respect `max_fps`, and advance while another animation drives the output. |
| `effect/window` | Backdrop sampling at rest, content shading inside an opening capture, the close snapshot, overview cards covering every mirrored surface, `window_effect` overrides, and the overlay following focus. |
| `effect/window_capture` | `in_capture` for grim and toplevel captures, a reload flipping both, one live instance after remap, and frames only for time-reading programs. |
| `effect/screen_cursor` | Screen override per output, the cursor square following the pointer, frames stopping when it hides or leaves, lock detachment, and both capture policies. |
| `effect/capture_feedback` | With `in_capture = false`, captured frames exclude the window effect from the first, display history never composites a capture frame, and display feedback counts the same animation instants as a run without capture. |
| `effect/reload` | Recovery from a missing shader, `[colors]` reaching a border palette without a recompile and a screen palette that does not read `umbriel_time`, and light layer removal and return. |
| `drag/physics` | Deformation while held, settling, rigid overview cards, handover to the close snapshot, and nothing left running with physics off. |
| `render/renderer_recovery` | The built-in fade recompiles once per renderer and an animation preset rebinds after recovery. |
