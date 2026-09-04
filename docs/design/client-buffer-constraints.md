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

## Capture readback format

`fx_texture_preferred_read_format` in `umbrielfx` never reports packed
24-bit. It is the only shm format the capture protocols offer clients, and the
NVIDIA blob reports `GL_RGB` / `GL_UNSIGNED_BYTE` for opaque targets, which maps
to `DRM_FORMAT_BGR888`. Clients assume a 4-byte pixel, derive `width * 4`, and
wlroots rejects it because a stride must divide by the pixel size. GLES2 always
allows `GL_RGBA` / `GL_UNSIGNED_BYTE` readback, so the clamp to 32-bit costs
nothing at 8bpc.

Do not fix this by dropping `DRM_FORMAT_BGR888` from `umbrielfx`'s pixel format
table: the table also drives `wl_shm` advertisement and texture upload, and it
is identical to wlroots' gles2 table.

To reproduce without NVIDIA, hardcode `gl_format = GL_RGB`,
`gl_type = GL_UNSIGNED_BYTE`, `alpha_size = 0` after the `glGetIntegerv` calls
and capture with `grim`.

## HDR capture view

Capture protocols negotiate their buffer constraints before they lock the
output for an attach-render frame. The SDR capture sidecar must therefore keep
the output buffer's DRM format while storing Gamma 2.2 SDR values. For an XR30
HDR output, replacing that sidecar with XR24 after negotiation makes the client
request packed 10-bit readback from an 8-bit framebuffer.

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

## Overview color mirrors

Overview cards use raw scene buffers that mirror each committed surface. A
normal scene surface is reset to wlroots' protocol-owned color state on every
commit. Umbriel repairs Wine compatibility descriptions at the render
boundary, but a raw overview mirror is not a scene surface and is not included
in that repair pass.

After copying ordinary scene-buffer properties, the overview must therefore
apply the compatibility manager's authoritative committed transfer function,
primaries, and luminance multiplier directly to its mirror. Otherwise a game
commit while overview is open reinterprets Windows scRGB as SDR and loses PQ
BT.2020 metadata.
