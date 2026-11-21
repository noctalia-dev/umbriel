# Client-requested background blur

`ext_background_effect_v1` lets a client name the part of its own surface that should show the backdrop blurred. Umbriel advertises the manager with the `blur` capability and applies the request as a clip on the blur node its owner already maintains; the user-facing keys are in the [appearance guide](../user/appearance.md#blur).

## Ownership

[`BackgroundEffectManager`](../../src/server/background_effect.h) owns the global, one object per surface, and each object's double-buffered region. `set_blur_region` writes the pending region; `wlr_surface` publishes it to the current state on commit, so a client can request and commit in one batch. The manager never creates scene nodes: `region()` reports what the current state asks for and each blur owner decides what to do with it.

Requests are read per surface, not per tree. `View` tracks `rootSurface()`, `Popup` tracks the popup's own surface, and `LayerSurface` tracks the layer surface; subsurfaces have no owner of their own and are not covered. Every owner re-reads the region when it refreshes its blur, and the `View` surface watch re-runs `updateBlur()` when the committed region changes, because a commit that only changes the region carries no new buffer.

Blur options resolve in [`SurfaceBlur::update`](../../src/scene/surface_blur.cpp): a non-empty request turns the blur on and clips it to the region, an empty request turns it off, and no request leaves the window or layer rule in charge. `appearance.blur.enabled` still wins over all three, and `appearance.blur.client_requests = false` drops the request before this logic so a client cannot opt itself in. A requested surface is drawn even when its opaque region covers the surface: the mask still hides opaque pixels, and the request is the client's own statement about what it draws.

## Rendering

The blur node keeps its full-surface box. `wlr_scene_blur`'s transparency mask stretches the masked buffer over the whole node, so shrinking the node would shift the mask against the content; the request is therefore a node clip, intersected with the shape clip the mask already contributes. A node clip is a scissor that SceneFX folds into visibility, damage, and hit testing, so no other geometry changes.

The region is surface-local, in logical pixels. `mapBlurRegionToNode` scales each rectangle by the node box over the surface box — an identity ratio in the steady state and the animation ratio while a window opens, closes, resizes, or enters fullscreen — and rounds the far edges outward so a fractional ratio cannot shave a blurred pixel. Blur alpha derives from the window's fade and the overview opacity only when a request is present, so a rule that leaves blur off does not suppress a request.

Optimized blur samples the cached background beneath the window stack, so over a bottom-layer surface it shows the blurred backdrop rather than a gradient; the requested region still bounds it. The harness check pins both the live and the optimized paths.

## Protocol build

`wayland-protocols >= 1.47` already ships `staging/ext-background-effect/ext-background-effect-v1.xml`, so the server and harness-client bindings are two more `custom_target` scanner rules in `meson.build`, with no new dependency. The global is added to the security-context allowlist in [`Server`](../../src/server/server.cpp): it exposes nothing about other clients, so sandboxed applications keep it.

`org_kde_kwin_blur` is not part of wayland-protocols and its XML is LGPL-2.1-or-later; it is not advertised. A client that only speaks that protocol still needs a window or layer rule for blur.
