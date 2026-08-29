# Border rendering

Umbriel renders each decorated window with one SceneFX `wlr_scene_border` node.
The node owns both color bands and submits one draw through the dedicated
`border.frag` shader. The regular rounded-rectangle shader is not part of the
window border path.

## Geometry contract

The final decorated outline is authoritative. A border node receives:

- the content box and explicit inner, seam, and outer corner radii;
- the inner and outer logical widths;
- the inner and outer premultiplied colors;
- a raster box large enough for the complete stroke and antialias coverage.

`makeBorderRing` adds one transparent logical pixel outside the configured total
width. This margin does not affect layout geometry or the visible border width.
It only ensures that a fractional outer sample is not clipped by the scene-node
quad before the fragment shader can evaluate it.

Scene rendering scales widths and the already-derived contour radii from logical
coordinates. The shader therefore receives physical geometry consistently, such
as a 1.25-pixel stroke on an output with 1.25 scale.

## Fragment contract

`border.frag` evaluates independent signed distances for three nested rounded
rectangles. Let the smooth nested radius at inset $d$ be:

$$
N(R, d) =
\begin{cases}
0 & R = 0 \\
\max\left(1, \operatorname{round}\left(\frac{R^2}{R + d}\right)\right) & R > 0
\end{cases}
$$

For configured outer radius $R$, inner width $I$, and outer width $O$, the
contours are:

1. outer edge: content box outset by $I + O$, radius $R$;
2. color seam: content box outset by $I$, radius $N(R, O)$;
3. content edge: content box, radius $N(R, I + O)$.

The rational curve equals $R$ at zero inset and remains positive for every
positive configured radius. It trades strictly constant corner thickness for
inner contours that decrease smoothly instead of abruptly becoming square.

Each contour uses the same Euclidean rounded-rectangle distance. Straight edges
and rounded corners remain continuous at their tangency. The two colors mix at
their shared boundary inside one fragment; they are never overlapping
transparent scene nodes.

Inner and outer edge coverage is symmetric around the corresponding geometric
boundary. A zero outer width selects the inner color for the complete stroke; a
zero inner width selects the outer color.

## CPU clipping

SceneFX limits fragment work by subtracting areas that are certainly inside the
transparent content hole. Rounded corners must remain shader-owned.
`apply_clip_region` uses the explicit content radii and subtracts only a central
horizontal and vertical cross. Integer truncation of a diagonal approximation
can otherwise remove an isolated fragment before the shader runs.

## Content coverage

The hole is transparent, so the client surface owns every pixel inside it. Both
boxes are scaled from the same logical edges, which is what keeps the surface's
physical box and the hole identical under fractional scaling. Nothing in the
render path may resize a surface's destination box to match its buffer: a client
that renders at the exact output scale can report a pixel count one short of
that box, and shrinking the destination leaves a line of background inside the
hole that neither the border nor the surface paints.

Texel alignment is recovered from the source box instead. SceneFX snaps a source
box to integer texels and then adopts the destination extent whenever the two
are within one texel, so sampling lands on texel centers rather than between
them, which is what keeps subpixel-hinted text sharp.

Snapping a source box outward can reach past the region it describes. A
client-side-decorated window is cropped to its xdg geometry, and under
fractional scale that crop edge falls between texels: the toolkit renders its
window into a pixel-aligned sub-rect and leaves the straddling texel to its
transparent shadow margin. Sampling it draws one see-through line along the
cropped edge. `fx_render_texture_options.sample_box` carries the whole texels the
source region owns, rounded inward, and selects a texture shader variant that
clamps every coordinate into it. The edge texel inside the crop is duplicated
instead, which is both opaque and sharp.

The clamp variant is confined to source boxes that cross an interior crop edge.
A source box reaching past the buffer itself, including the deliberate one-texel
destination adoption above, already duplicates its edge texel through
`GL_CLAMP_TO_EDGE` and stays on the regular shader. This split is deliberate:
clamping inside every texture fetch caused constant full-session flicker and
tearing on NVIDIA (595.71.05, open modules), so an empty `sample_box` must keep
the fetch byte-identical to a build without the clamp.

## Scene lifecycle

View decorations, overview cards, and their close-animation snapshots all copy
or animate one `wlr_scene_border`. Focus animation updates the inner color;
opacity animation updates both premultiplied colors. The outer color never needs
a second scene node or draw order.

## Regression coverage

- `365_fractional_border_coverage.sh` checks a one-logical-pixel border at scale
  1.25. The top and side must have equal opaque and fractional coverage.
- `366_fractional_content_coverage.sh` pins a float at scale 1.25 whose floored
  buffer is one physical pixel short of its content box on both axes. The last
  column and row inside the box must be client content, and an interior run must
  hold no blend of the client's alternating columns.
- `367_csd_crop_edge_coverage.sh` pins a float at scale 1.25 whose surface
  carries a transparent one-logical-pixel margin around its declared window, so
  the crop lands a quarter texel off the grid on every side. All four edge lines
  inside the content box must be window content.
- `722_subsurface_border_corner.sh` checks the outer arc, smooth two-color seam,
  positive content radius, and straight-to-curve tangency against a full-window
  subsurface.
- `723_small_border_corner.sh` verifies that a one-pixel outer radius does not
  grow with a thick double border, an eight-pixel radius keeps its inner contour
  rounded, and zero preserves a square outer corner.
- `border-ring` unit tests protect the transparent raster margin and content-hole
  geometry.

When changing border geometry or antialiasing, temporarily break the relevant
invariant and confirm its regression check fails at the intended sample.
