# Nine-rect rendering

The optional textured frame uses three layers: immutable decoded theme data in
`src/config/nine_rect.*`, per-window scene geometry in `src/scene/nine_rect.*`,
and source-region sampling in `umbrielfx`. Configuration and asset validation
complete before `ConfigStore` commits. Renderer texture imports remain lazy,
as with other scene buffers; decoding and geometry require no GPU.

## Geometry

The destination center defaults to the client box. Optional `content_inset` or
`content_inset_px` places the client edge inside the source slice bands, allowing transparent inner
padding to overlap the client while preserving corner size and tile period.
The shared destination-cut calculation also compresses corners for tiny clients.
Source pixels correspond to logical pixels before normal output scaling.
Fractional slices and content insets resolve once, with half-up rounding.
Empty source regions produce no quads or input and never enter a modulo/division operation.

The content insets also define the rectangular logical frame used by layout
and shadows. No semantic image or per-pixel geometry is maintained.

Nine-rect mode makes layout engines allocate logical frame slots with only the
configured gap. `Workspace::tiledTargetBox` converts slots to client rectangles
using the four logical insets. Initial configure sizes and IPC geometry use
that same convention. Floating anchors and generated shadows also account for
these insets.

Client dimensions remain subject to application size hints. Visual dimensions
are client dimensions plus the content insets. Corners compress when that box
is too small to contain the fixed slice bands, preventing inverted geometry
and overlap. Small or overconstrained layouts retain the existing
client-constraint policy. Fullscreen and maximized-to-edge modes
keep their existing undecorated behavior.

## Sampling and ownership

Up to nine scene buffers share immutable source pixels. Each has a source
rectangle and two repeat counts. A count of one stretches; a tiled band uses
its destination logical extent divided by its source extent. `tex_slice` maps
coordinates into that subrectangle and crops incomplete repetitions. Its
sampling clamp cannot read a neighboring slice. The existing surface shaders
retain their ordinary sampling paths.

Input callbacks test destination coordinates against the client rectangle.
Frame bands accept resize input regardless of artwork alpha; overlapping client
regions and center overlays pass input through. This remains true when corners
compress for tiny windows or overview scales the geometry. Decoration hits
resolve to their owning view, and existing layout edge restrictions apply.

Unlike ordinary uploaded scene buffers, slice buffers retain their source lock
after GPU upload. This permits renderer recreation and allows close snapshots
to hold the same immutable resource generation after a config reload. Snapshot
copies retain source boxes, repeat counts, filter, tint, and opacity, and reject
input.
Textured snapshot chrome stays outside the client clip and participates in
shrinking geometry and fade opacity. Overview cards build scaled geometry from
the same source generation.

## Effects and damage

Artwork lives above the client. Procedural widths and radii do not affect it,
and client surfaces remain square. Blur uses the ordinary client effect nodes,
parameters, and window rules; decoration does not add blur nodes or shader paths.
Scene damage, visibility, output clipping, and capture use the frame bounds.
Assets remain immutable until reload; no PNG reads occur per frame.

Optional `tint_with_border_color` uses a per-buffer straight RGBA multiplier in
the slice shader. Color changes damage the scene node, without decoding or
uploading new artwork. The existing border-color animation supplies live tint;
overview selects its card color, and snapshots copy the multiplier.

## Regression coverage

- Unit tests cover asset validation, fractional slices and content insets,
  rectangular input, tiny-window geometry, tint, and snapshot ownership.
- `369_nine_rect.sh` compares a complete tiled edge pixel by pixel, checks center
  overlay and frame bounds, rejects a bad live reload, resizes through the frame,
  verifies output scales 1.25 and 2, and preserves chrome during close animations.
- `370_nine_rect_blur.sh` verifies blur stays inside the client area and follows
  window rules even when the border texture is transparent.
- `371_nine_rect_layout.sh` verifies asymmetric logical frames across scrolling,
  dwindle, and master layouts, plus textured overview cards.
- `372_nine_rect_content_insets.sh` checks content touching each border and
  modifier drags preserving window size.
- `373_nine_rect_drop_animation.sh` samples held, released, and intermediate
  animation frames for both edge drops across all three layouts.
- `374_nine_rect_tint.sh` verifies albedo/alpha multiplication, focus, theme reload,
  disabling tint and tinted close snapshots.

The ordinary procedural-border, configuration, scene ABI, and renderer suites
remain applicable when nine-rect mode is disabled.
