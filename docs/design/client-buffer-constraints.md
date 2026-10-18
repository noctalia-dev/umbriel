# Client buffer constraints

Clients size and format their own `wl_shm` buffers from what the compositor
tells them. Getting either wrong is a protocol error, so the client dies.

## Initial layer-surface configure

`LayerSurface::handleCommit` arranges the output synchronously on
`initial_commit`. A client that requests 0x0 learns its size from that first
configure; deferring it to the next frame lets the client allocate first and get
`Invalid size (0)`.

The `Dirty::LayerArrange` flag recorded alongside is the retry:
`Output::arrangeLayers` returns early while the output has no effective
resolution.

## Stale window geometry

`View::committedContentBox` widens a tiled view's committed window geometry
towards the size the view was configured to, capped by what the surface
actually holds from the geometry origin. Presentation, the surface clip,
borders, blur, shadow, the resize-animation snap, and the size the `windows`
IPC listing reports all read that box.

Electron acks a configure and redraws at the new size while leaving
`set_window_geometry` at the size it had before, permanently. Clipping to that
box crops the content the client just drew and leaves the window occupying a
fraction of its tile, with no resize available because the tile itself is
already correct. Growing the box is bounded by the surface, so a client that
genuinely refuses the configured size, a minimum-size hint for instance, is
still presented at the size it committed.

## Capture readback format

`fx_texture_preferred_read_format` in `umbrielfx` never reports packed 24-bit or
packed 10-bit for shared-memory capture. For XR30 and XB30 targets it publishes
XRGB8888 when BGRA readback is available, and XBGR8888 otherwise. GLES performs
the depth conversion during readback. The NVIDIA blob reports `GL_RGB` /
`GL_UNSIGNED_BYTE` for opaque 8-bit targets, which maps to
`DRM_FORMAT_BGR888`. Clients assume a 4-byte pixel, derive `width * 4`, and
wlroots rejects it because a stride must divide by the pixel size. GLES2 always
allows `GL_RGBA` / `GL_UNSIGNED_BYTE` readback, so publishing a 32-bit format
avoids that mismatch.

Do not fix this by dropping `DRM_FORMAT_BGR888` from `umbrielfx`'s pixel format
table: the table also drives `wl_shm` advertisement and texture upload, and it
is identical to wlroots' gles2 table.

Explicit texture readback must also accept the bound framebuffer's
`GL_IMPLEMENTATION_COLOR_READ_FORMAT` / `GL_IMPLEMENTATION_COLOR_READ_TYPE`
pair. NVIDIA supports AB30 readback without advertising its texture-upload
extension. Applying only the upload capability check rejects valid 10-bit
readback and prevents the FP16 renderer tests from checking their pixels.

To reproduce without NVIDIA, hardcode `gl_format = GL_RGB`,
`gl_type = GL_UNSIGNED_BYTE`, `alpha_size = 0` after the `glGetIntegerv` calls
and capture with `grim`.

## HDR shader precision

Client texture conversion and the output transform both require unconditional
`highp float`. This preserves the output shader as the renderer-wide precision
gate: implementations without fragment highp cannot initialize the renderer,
and HDR is never exposed with arithmetic known to be insufficient.

On NVIDIA 610.57.04, relying only on the shader macro selected mediump and
corrupted PQ decoding: `color-pq-roundtrip` returned red 90 instead of 96.
Forcing highp fixed the test, while explicit highp samplers alone did not.

GLES3 detection remains limited to the packed 2_10_10_10 texture type, which
is core functionality in GLES3. The capability is enabled by either the
context version or the extension string. Framebuffer readback retains its
separate implementation-format check.

## HDR output transitions

Every enabled configured-state commit must carry a scene buffer built for that
same pending output state. This is especially important for HDR image-description
and render-format changes. Without a buffer, wlroots may supply a cleared buffer
for the reconfiguration, and a following nonblocking scene commit can race the
DRM modeset and leave that cleared buffer visible.

Dynamic HDR policy changes therefore schedule a full frame. At the frame
boundary, `Output::applyConfiguredState` enables reconfiguration, damages the
whole output, builds the scene into a copy of the staged state, requires both
`WLR_OUTPUT_STATE_BUFFER` and a nonnull buffer, then commits the state and
rendered frame together. Output creation also creates its scene output before
initial configuration, preserving this invariant when DRM outputs are recreated
after resume.

All three format tiers, including 8-bit SDR, use backend pretests before scene
building. On DRM, wlroots prepares and tests a buffer when the pending
state requires one. A rejected pretest advances the format, VRR, or preferred-mode
fallback sequence. Testing the final 8-bit candidate prevents a rejected primary
swapchain from surfacing only as a scene build failure and suppressing mode
fallback.

After an accepted pretest, a pending-state copy failure, scene build failure,
missing buffer, or temporary neutral-mask failure remains retryable. It stops the
format sequence without trying SDR, VRR, or mode fallbacks. The requested policy
remains pending for the delayed frame retry because none of these preparation
failures proves that the staged output format is unsupported.

A backend commit failure is instead treated as a rejection and retains the
existing format fallback behavior. If a fallback commits successfully,
`HdrTransition` records the requested policy as handled and reports the fallback
reason instead of retrying HDR every frame. If no candidate commits, the
transition remains pending for a later frame.

A full geometry commit made while the session is locked, or before the output is
bound to the logical output layout, renders through a temporary opaque
backdrop-colored mask. Its logical extent is derived from the pending mode,
transform, and scale, and it remains above the scene through state building and
the backend commit. Grouped output-management commits use the same protection.
This prevents an unbound output from sampling the scene origin and prevents a
geometry change from exposing content beyond a lock blank that has not yet been
resized.

`output-hdr-transition` covers transition bookkeeping, build-before-commit
ordering, and the rendered-buffer guards. `output-format-sequence` covers the
distinction between retryable preparation failures and backend rejection. The
headless color-management check covers policy entry and release, but a real HDR
monitor is still required to validate the KMS transition.

## HDR white-point adjustment

Gamma-control clients use the same interface in SDR and HDR. In HDR,
`hdrWhitePointFromGamma` extracts the normalized, sRGB-encoded endpoint from
each channel's latest gamma ramp. A missing table is neutral, while dimmed or
black endpoints retain their requested brightness. Identity ramps sampled with
either `i / (N - 1)` or `i / N` are recognized across every entry, allowing one
16-bit unit of rounding. This prevents the latter sampling style's sub-white
endpoint from enabling an adjustment when the full ramp is neutral. A custom
curve with the same sub-white endpoint remains an adjustment.

UmbrielFX decodes those endpoints to linear sRGB and derives a Bradford
adaptation from D65 to the requested white without normalizing away its
luminance. The adaptation is composed with the existing output matrix before
PQ encoding. It preserves HDR highlight headroom instead of clipping the image
to SDR white; individual channel brightness changes with the requested warmth.
The full LUT curvature is never applied to PQ values. Umbriel retains the
client's latest full table, which is applied again on return to SDR.

Changing the white point damages the whole output and schedules a frame. It
invalidates the output transform only while the output is PQ; an SDR output
keeps its transform, since rebuilding it would move composition onto the
linear blend path, and a pending PQ description rebuilds it with the stored
white point. A non-neutral adjustment on a PQ output disables
direct scanout and locks software cursors so the cursor receives the same
display transform. State building sets the cursor policy from the pending
output description before collecting cursors, then restores the live policy
until commit. Commits, neutral restoration, SDR transitions, disabled outputs,
and destruction balance that lock.

The SDR capture sidecar is generated before this display transform and excludes
the adjustment. Export-DMA-BUF retains the adjusted native HDR representation.
Math and render checks cover extraction, adaptation, HDR transitions, and the
unadjusted SDR capture. Physical HDR display verification must also check warm
and neutral transitions, highlight gradients, cursor matching, and return to
SDR with the full gamma curve restored.

## Implicit scene-buffer primaries

A raw `wlr_scene_buffer` uses zero primaries as an internal unset sentinel.
This can remain visible while a new surface or a copied snapshot enters an
animation, before protocol color metadata has been applied. Unset primaries use
the sRGB primaries.

The scene render boundary snapshots the named value and resolves zero to sRGB
before calling `wlr_color_primaries_from_named`. The converter remains strict
for invalid named values. `color-scene-unset-primaries` builds a composited
frame from a raw scene buffer and covers this boundary; removing the resolution
causes the check to abort in the converter.

## Explicit surface transfer functions

A scene buffer's zero transfer function means unspecified SDR. Surface helpers
retain that sentinel when a client has no committed color description, including
after unsetting or destroying its color object. Snapshot and overview copies
preserve it. The renderer selects the pass's SDR curve only for unspecified
content: the output's `sdr_transfer` choice on PQ, gamma 2.2 on gamma 2.2
outputs, and piecewise sRGB otherwise. The PQ default remains gamma 2.2.

Explicit transfer functions retain their meaning. In particular, protocol v2's
`compound_power_2_4` maps to `WLR_COLOR_TRANSFER_FUNCTION_SRGB` and must use the
piecewise curve even on a PQ output. This is distinct from the deprecated,
ambiguous protocol `srgb` identifier. Gamma 2.2, PQ, and linear descriptions are
also decoded as declared. Compositor colors still follow the pass's SDR policy.

The scene boundary resolves the unspecified transfer function to sRGB only when
looking up nominal SDR luminance. Direct scanout also recognizes unspecified
SDR with sRGB primaries on an sRGB target, preserving its existing eligibility.
Neither operation replaces the sentinel passed to the renderer.

`color-scene-unset-primaries` checks pixel transitions from unspecified SDR to
explicit piecewise sRGB and back on PQ. `color-sdr-curve-consistency` checks
explicit and implicit decoding independently from compositor colors and verifies
both SDR capture paths. On PQ outputs, captures use the pass's selected SDR
encoding, so an explicitly tagged source can produce different captured code
values while preserving its decoded luminance. The `render/color_transfer`
harness check uses real protocol v2 descriptions across top-level and
synchronized or independent subsurface commits, including color-object
destruction and recreation.

The per-output SDR policy is stored in a separate `wlr_output` addon. It survives
renderer recovery and scratch-buffer recreation without extending shared
wlroots structs. Configuration applies it before building an output's initial
or reconfigured frame. A changed choice fully damages the output, invalidates
the blend and capture generations, clears effect buffers, and marks intersecting
optimized-blur nodes dirty. The blend and optimized-blur cache keys also include
the selected curve. An unchanged choice adds no scene damage.

`color-sdr-transfer-policy` renders two PQ outputs with opposite choices and
swaps their policies. It checks explicit metadata, compositor colors, optimized
blur, both SDR captures, redraw outside prior partial damage, and policy
retention across scratch-buffer destruction. Non-PQ targets retain their
existing curves. The `render/sdr_transfer` harness check covers per-output
configuration reloads, default restoration, and unchanged SDR fallback pixels.

## HDR capture view

Capture protocols advertise their shared-memory and DMA-BUF constraints before
they lock the output for an attach-render frame. For XR30 and XB30 sources,
shared-memory capture is advertised as 8-bit and GLES converts the SDR sidecar
during readback. The sidecar itself must continue to use the output buffer's
DRM format because DMA-BUF capture may already have negotiated that 10-bit
storage, and texture substitution must remain storage-compatible. Its stored
values use the output's selected SDR transfer curve, gamma 2.2 by default, even
when its DRM format is 10-bit.

Each output owns one shared FP16 blend buffer and one shared SDR capture
sidecar. Their lifetime is tied to that output, rather than to its swapchain
buffers, so changing swapchain depth does not duplicate either allocation. The
blend buffer always represents the latest complete composed frame. Incremental
rendering updates the region drawn for the current frame while preserving the
previous frame everywhere else.

Creating or resizing the shared blend buffer requires whole-output damage for
that frame. A new buffer is cleared and has no valid pixels outside partial
damage, including pixels that blur may sample. The full redraw establishes the
complete-frame invariant before incremental damage resumes. An untransformed
composition or direct scanout bypasses the shared target and invalidates it, so
the next transformed composition also performs a full redraw.

`umbrielfx` creates the capture sidecar lazily from the pre-output-transform
linear blend buffer. The sidecar is generation-matched to that blend buffer:
capture can reuse it only when the requested output buffer has the same blend
generation, dimensions, and DRM format. A conversion for a newer generation
does not overwrite the sidecar while a capture texture still holds it. This
conversion can happen during texture import, outside the normal `umbrielfx`
render pass, so its EGL context must be current while the framebuffer is
allocated. Export-DMA-BUF frames bypass the SDR sidecar and retain the output's
native representation.

Output color LUTs are cached as renderer-local textures keyed by the immutable
LUT transform. Reusing an output transform therefore reuses its uploaded
texture, while transform and renderer destruction both release the cache entry.

## Client color descriptions

Umbriel's `ColorManager` owns the color-management protocol for native and Wine
clients. Native clients receive a version 2 global with the ordinary supported
feature set. Wine clients receive a separately filtered compatibility global
that also exposes Windows-scRGB and, when available, Windows BT.2100. Both
globals share surface state and the scene color resolver.

Surface descriptions use `wlr_surface_synced` so cached commits and synchronized
subsurfaces apply color metadata with the matching buffer. Destroying a
`wl_surface` leaves its color-management resource inert. Surface preferences
follow actual output membership, preferring an HDR output when a surface spans
SDR and HDR outputs.

For an active PQ output, output descriptions and preferred surface descriptions
advertise the configured `sdr_white` as their reference luminance. The primary
PQ range remains 0.005 to 10000 cd/m²; EDID primaries and luminance limits
describe the independent target color volume. SDR output descriptions retain
their nominal 80 cd/m² reference white.

Description comparisons include reference luminance. A reload that changes only
`sdr_white` therefore notifies output-description and surface-feedback clients
while HDR is active, even when the transfer function and primaries stay the
same. Clients obtain the updated values by requesting a new immutable
description. Unchanged descriptions produce no notification.

## Windows-scRGB luminance

Windows-scRGB and generic extended-linear content share the same transfer
function, but not the same reference-white convention. Windows-scRGB defines a
linear value of 1.0 as 80 cd/m2 and uses 2.5375, or 203 cd/m2, when compositor
processing needs an assumed reference white. Treating every extended-linear
buffer as Windows-scRGB would incorrectly dim parametric extended-linear
content.

Umbriel therefore marks only image descriptions created by
`create_windows_scrgb` with an `umbrielfx` luminance multiplier of `80 / 203`.
`umbrielfx` applies that multiplier while normalizing the buffer into its
reference-white-relative blend space. The output transform subsequently maps
the normalized reference white to the configured output `sdr_white` level.
Buffers using this multiplier cannot use direct scanout because scanout would
bypass the conversion.

## Scene color mirrors and snapshots

Scene surfaces resolve their committed transfer function, primaries, and
luminance multiplier during surface reconfiguration. Umbriel's color resolver
overrides the natural wlroots state once, before guarded scene setters run, for
both native and Wine clients. Unchanged descriptions and callback-only commits
leave the existing color state intact. Synchronized subsurfaces resolve only
their applied state, keeping cached metadata aligned with its buffer.

`ColorManager` registers this resolver on the main scene and every isolated
window capture scene before adding surfaces. Each registration follows the
scene root's lifetime: root destruction removes its listener and resolver,
and manager teardown clears the resolvers on any remaining roots before
freeing their shared state. Window capture therefore resolves the same client
metadata as the desktop while rendering only the captured client's surfaces.

Destroying a color-management object refreshes matching scene surfaces
immediately across all registered scene roots, including disabled scene trees,
without requiring another surface commit. `render/color_damage` checks unchanged
commits, delivered callbacks, real color transitions, object destruction, and
synchronized and desynchronized subsurfaces through captured pixel damage.
`render/window_capture_color` compares numeric output and isolated window
capture pixels through transfer-function and primaries changes, synchronized
commits, color-object destruction without a commit, and client teardown.

Overview cards, desktop mirrors, close snapshots, and resize snapshots use raw
scene buffers outside the surface resolver. Their copy paths therefore apply the
manager's authoritative committed description directly after copying scene
properties. This preserves PQ BT.2020 and Windows-scRGB metadata even when a
copy is created directly from a surface commit.
