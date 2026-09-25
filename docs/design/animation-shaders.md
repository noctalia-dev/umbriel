# Custom animation shaders

Custom fragment shaders are supported on every animation event. Configuration
and authoring details are in [Animation](../user/animation.md#custom-glsl-shaders).

## Configuration and compilation

`readAnimationShader` uses a `Section` reader for the `shader` file path.
Inline GLSL is not accepted. Paths resolve relative
to the declaring TOML file, including included files. The loader registers file
dependencies even when missing, and includes source contents in configuration
equality. Validation rejects blank/NUL text, nonregular files, and inputs larger
than 256 KiB. Nonblocking file opens prevent FIFOs hanging config reload.

The C++ scene adapter caches one program per event, exact source, and renderer.
Startup and animation config reload prepare programs before rendering. Failures
are cached too, avoiding per-frame compiler retries. UmbrielFX supplies a GLSL
ES 1.00 wrapper around `vec4 animation(vec2 uv)`, normalized target sampling,
target-local previous-result sampling, a stable four-channel random seed,
logical target size, eased and linear progress, and transition direction.
Compiler diagnostics retain source line numbers and the file/event label.

Without a custom shader, `windows_in` and `windows_out` bind a built-in fade
program through `lifecycleShader`, except for the `slide` style, whose opacity
curve differs from its progress. The window, its subsurfaces, and its border
are composited once and faded as a group, so overlapping surfaces never show
through each other mid-fade. `View::fadeComposited` and the close snapshot keep
buffer and border opacity at 1 while such a program runs. The built-in program
is marked shape-preserving, so its window keeps the analytic shadow, which the
view fades itself. Scratchpad and layer fades stay per buffer, because they can
start from a partial alpha that normalized progress does not carry.

## Scene processing

Effect state is attached through scene-node addons, preserving the scene ABI.
Nine ordered slots permit simultaneous effects on a node. Descendant effects
run before ancestor effects; same-node order is dimming, border, movement,
opening, closing, scratchpad, layers, workspaces, then overview.

| Event | Target and timeline owner |
| --- | --- |
| `windows_in` | View tree and existing map fade |
| `windows_out` | Close snapshot, including a card closed from overview, and existing close fade |
| `windows_move` | View tree and position/presentation-size animation |
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
submitted post-shader result for the same node, slot, output, and renderer. The
first render uses the current input. History uses normalized target coordinates,
so it follows movement and is resampled across target-size changes. A transition
ID distinguishes retargets from spring progress moving backward. Snapshot
creation preserves that ID and seed and transfers feedback history before the
source target is retired.

Previous-result feedback lazily allocates two target-sized buffers per active
node, slot, and output. The pair costs 8 bytes per pixel in SDR and 16 bytes per
pixel with an FP16 color-management target, before allocator overhead. History
is reset for a new transition, program, output transform, working format, or
renderer. Allocation or import failure disables feedback for that transition
and keeps direct shader rendering available. Front-buffer promotion is deferred
until render-pass submission succeeds. Shadow silhouette captures may read the
same prior result but never advance it.

Intermediate buffers are pooled per output and nesting depth, allocated on
demand and dropped after effects end. Composition preserves the working format,
including FP16 when color management uses a linear intermediate. Blur within
an effect samples a reconstructed backdrop containing outer captures and earlier
siblings. Texture imports are checked before capture; allocation/import failure
leaves ordinary rendering available. Capture depth is bounded at 24.

While a scene has active effects, opaque-region culling and direct scanout are
disabled and outputs receive full damage. This conservative policy allows
arbitrary target sampling and changes in alpha without stale pixels.

## Lifetime

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
while the fade centres a scaled presentation inside it on every tick, and the
layout paths record the resting origin instead of animating toward it. The
fullscreen backdrop is the window's own letterbox and follows the presented box,
so the opener scales with its surround rather than inside an output-wide one.

A normal tiled close snapshots the complete decorated view, drops any copied
movement effect, and is appended above every workspace tree under the output's
view root. It is a fixed canvas: `CloseSnapshot::present(canvasX, canvasY,
visible)` only translates it with its workspace's slide and hides it while that
workspace is not showing. `umbriel_size` is therefore constant for a close
snapshot unless the built-in `popin` or `zoom` style shrinks it, which scales
the frozen buffers, borders, and shadow toward the captured box's centre using
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
and a cancelled size animation discard the capture.

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

## Regression coverage

`tests/unit/animation_shader.cpp` exercises source validation and file reads.
`tests/unit/config_load.cpp` covers all event sections, included-file provenance,
source-content reload effects, dependency deduplication, and dependency removal.
`tests/unit/animation.cpp` verifies that tiled geometry preserves safe curves and
projects overshooting or reversing curves into bounded monotonic full-duration
travel. `tests/unit/layout_motion.cpp` covers shared-progress interpolation and
the separation test that orders a rearrangement's raises.

The isolated running-compositor checks `180_animation_shaders`,
`181_animation_shader_events`, `182_animation_shader_composition`,
`192_tiled_close_lifetime`, `193_tiled_open_reflow_timing`,
`194_tiled_close_no_reflow`, `195_tiled_open_shader_box`,
`196_tiled_lifecycle_move_timing`, `198_close_snapshot_workspace`,
`200_tiled_close_retained_no_reflow`, `202_tiled_close_shader_visibility`,
`205_tiled_close_configure_barrier`, `206_tiled_close_fixed_snapshot`,
`207_tiled_repeated_close`, `208_dwindle_many_close_timing`, and
`330_overview_close_fade` inspect
shader-specific intermediate pixels, file-watcher reloads, every animation
event, both layer lifecycle directions, rotated fractional-scale UVs, nested
sampling, output containment, invalid-GLSL fallback, and a tiled close effect
that outlasts its configured `windows_move` timeline. Checks 193 and 195 also
verify that a tiled opener runs its own `windows_in` in its final slot on the
same tick its established neighbours begin to reflow, with each clock keeping
its own duration.
Check 198 covers tiled and non-layout close snapshots following workspace
translation and visibility while their independent lifecycle continues.
Check 200 keeps a no-reflow close fixed through an unrelated opening and
consume. Check 202 verifies that ordinary, consume, expel, and
disabled-movement closes preserve natural early, middle, and late shader phases
while survivors start moving at once. Check 205 verifies immediate target
configures and immediate visible movement, plus compositor endpoint ownership
for a client that retains stale buffer dimensions across a later stable
arrange. Check 206 covers longer, equal, and shorter `windows_out` duration
orderings against `windows_move` and asserts that the snapshot holds its
captured box for every sampled frame. Check 207 interrupts scrolling reflow
with a second close and verifies independent shader phases, monotonic survivor
motion on a single movement clock, and no summed delay or starvation. Check 191 fades a window whose opaque subsurface covers its parent and verifies
that the parent never shows through while opening or closing. Check 209 redraws a reflowing neighbour in a new colour at its new size and
observes blended pixels mid-reflow, then the redrawn frame alone. Check 208 closes
the root leaf of a five-window dwindle tree and verifies that every changing
survivor follows an overshooting movement curve across its full configured
clock instead of pinning at its first endpoint crossing. The overview check
verifies that a card close remains outside workspace vacancy coordination and
that the closing card drops its copied movement effect before `windows_out`
samples the captured client buffer. The `183_animation_shader_lifetime` and
`184_animation_squash` checks also cover
program retention across reloads, close-during-open snapshots, shader removal,
and the bundled squash effect's intermediate pixels. Bright-green shadow
assertions in `184_animation_squash`, `185_animation_shadows`,
`186_animation_shadow_lifetime`, `187_animation_shadow_composition`, and
`188_animation_shadow_border` cover
shader-created edges, normal-shadow restoration, closing teardown, rotated
fractional-scale offsets, reload and close-during-open retention, stacking,
translucent interior exclusion, enclosing workspace effects, descendant-only
border effects, retained closing borders, and smooth blur gradients. Negative controls
temporarily bypass shader rendering and configuration assignment to ensure
the checks fail for the behavior they cover. Shadow negative controls bypass
silhouette rendering in a separate temporary build, never in the working source.

`189_animation_shader_feedback` verifies first-frame initialization, recurrence,
target isolation, and feedback transfer into a closing snapshot.
`190_animation_shader_seed` verifies all four channels, stability within a
transition and its closing snapshot, and renewal when the same client maps
again. Unit coverage verifies global transition identities and preservation
while underdamped spring progress reverses. Negative controls replace feedback
with current-target sampling, force a reused seed, suppress transition renewal,
and refresh spring identity on every tick. Each check fails on its intended
behavior.

Headless checks do not establish physical HDR output correctness or hardware
GPU-reset recovery. Those require suitable hardware and a running-session check.
