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

`fx_texture_preferred_read_format` in the SceneFX fork never reports packed
24-bit. It is the only shm format the capture protocols offer clients, and the
NVIDIA blob reports `GL_RGB` / `GL_UNSIGNED_BYTE` for opaque targets, which maps
to `DRM_FORMAT_BGR888`. Clients assume a 4-byte pixel, derive `width * 4`, and
wlroots rejects it because a stride must divide by the pixel size. GLES2 always
allows `GL_RGBA` / `GL_UNSIGNED_BYTE` readback, so the clamp to 32-bit costs
nothing at 8bpc.

Do not fix this by dropping `DRM_FORMAT_BGR888` from the fork's pixel format
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

SceneFX creates the sidecar lazily from its pre-output-transform linear blend
buffer. This can happen during texture import, outside the normal SceneFX
render pass, so the SceneFX EGL context must be current while the framebuffer
is allocated. Export-DMA-BUF frames bypass the SDR sidecar and retain the
output's native representation.

## Windows-scRGB luminance

Windows-scRGB and generic extended-linear content share the same transfer
function, but not the same reference-white convention. Windows-scRGB defines a
linear value of 1.0 as 80 cd/m2 and uses 2.5375, or 203 cd/m2, when compositor
processing needs an assumed reference white. Treating every extended-linear
buffer as Windows-scRGB would incorrectly dim parametric extended-linear
content.

Umbriel therefore marks only image descriptions created by
`create_windows_scrgb` with a SceneFX luminance multiplier of `80 / 203`.
SceneFX applies that multiplier while normalizing the buffer into its
reference-white-relative blend space. The output transform subsequently maps
the normalized reference white to the configured output `sdr_white` level.
Buffers using this multiplier cannot use direct scanout because scanout would
bypass the conversion.
