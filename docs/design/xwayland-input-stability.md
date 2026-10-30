# Xwayland input stability

Umbriel supports X11 clients through wlroots' Xwayland server: managed X11 windows are views, and the compositor tells the X server where each one sits on screen. X11 games are sensitive to compositor-side geometry churn, and several Umbriel invariants exist to keep it away from them. Breaking any of these reintroduces a bug class where an X11 game's mouse input dies in part of the screen while everything looks correct.

## Association catches up with the first buffer

Xwayland sends window association messages over the X11 connection and surface commits over its Wayland connection. The first committed buffer can therefore arrive before the XWM associates that surface with its X11 window. wlroots installs its mapping commit listener during association, so it can miss that first buffer and leave the surface unmapped. An idle application can then wait indefinitely for a frame callback from a surface that never entered the scene. The X window remains viewable and in the stacking list while `_NET_CLIENT_LIST` and Umbriel's window list omit it.

`XwaylandWindow::handleAssociate` creates the managed or unmanaged wrapper and installs its listeners before checking the currently associated surface. If that surface has a committed buffer and is still unmapped, it calls `wlr_surface_map` immediately. This delivers the same map signal to wlroots and Umbriel that a later buffer commit would deliver, including the client-list update. Pending buffers are left to the normal commit path, and an already mapped surface receives no duplicate map signal. Dissociation still unmaps and destroys the wrapper before another association is handled.

`protocol/xwayland_association` exercises real Xwayland with an idle X11 client and a proxy on its private XWM connection. The proxy holds the selected association message while Wayland traffic continues. The check waits for the first buffer import, releases association, and requires the existing buffer to map without a redraw. It also checks ordinary ordering, managed window withdrawal and remapping, and rendered pixels from a delayed unmanaged window. Removing the catch-up must fail the delayed mapping transition.

## Popup presentation waits for XWM geometry

An associated Wayland surface can already have a buffer while the XWM still has an older X11 position cached. Association reads window properties through libxcb, which can move unrelated `ConfigureNotify` events from the socket into its internal queue. The socket then stops being readable even though the XWM has events left to dispatch. In wlroots 0.20.2, its registered event-source recheck does not drain that queue when Wayland calls it with a zero event mask. A later X11 event can finally apply the missing geometry. Until then, a popup can remain drawn at its old position even though an independent X11 geometry query reports its new location.

Unmanaged windows therefore begin with their scene tree disabled. Each map requests a tagged `ClientMessage` on an InputOnly window owned by Umbriel's existing XWM connection. When the XWM dispatches that marker, it has processed the earlier queued geometry events. The matching popup then synchronizes its position again, becomes visible, and applies its current keyboard-focus policy. Geometry and shape updates can still update the hidden tree while it waits.

The marker carries the X window ID and a unique per-map serial. Completion is routed through the current window wrapper rather than a stored callback into an object that may have been destroyed. Unmap cancels the pending serial; remapping or replacing a wrapper requests a new serial, so an old marker cannot reveal the wrong surface. Lazy-server teardown invalidates the synchronization window and connection.

This uses one asynchronous X-server roundtrip per unmanaged map, with no extra X client, blocking reply wait, or settling timer. It orders presentation after already queued XWM work; it cannot predict a position the client requests after the marker. The first-buffer association catch-up remains necessary for idle clients and is independent of when the unmanaged scene becomes visible.

[`protocol/xwayland_initial_position`](../../tests/harness/checks/protocol/xwayland_initial_position.sh) holds a real X11 geometry event and its presentation marker while an already committed Wayland buffer is ready. Continuous output capture must never show the obsolete centered position. Releasing the geometry before the marker must produce the first visible frame in the corner. A deliberately centered client verifies the capture observer, and a pending unmap/remap verifies that the first map's marker cannot reveal the second map. The check covers override-redirect and notification windows, including both map/move request orders.

## Popup classification follows live X11 metadata

Override-redirect windows and windows typed as notifications, popup menus, dropdowns, tooltips, combo popups, or drag icons use `XwaylandUnmanaged`. They keep client placement and bypass managed window rules. So does a utility window transient for another X11 window whose `WM_HINTS` refuses keyboard input: the client places it and typing stays in the window it belongs to, as with WeChat's search suggestions. EWMH `_NET_WM_WINDOW_TYPE_MENU` describes a persistent torn-off menu, so that type remains managed unless override-redirect is set. Its window rules, workspace membership, overview card, and IPC identity follow normal managed behavior. `XwaylandWindow` watches both override-redirect and window-type changes, switching wrappers immediately when their classification changes, including while mapped. Metadata updates that keep the same classification preserve the current wrapper.

While managed, the window retains its client-requested X11 rectangle separately from compositor layout configures. Conversion to a popup restores that rectangle and clears maximize and fullscreen state. Direct client geometry changes accompanying an override-redirect transition are retained. Returning to managed classification seeds the saved rectangle from current popup geometry and applies normal opening policy.

Wrapper changes preserve the Wayland surface's mapped state. A retiring View runs full unmap cleanup before destruction, including layout, focus, cursor grabs, effects, and overview cards, without creating desktop or overview close snapshots. A replacement View enters the registry before catching up with an already mapped surface. The XWM focus record is retired only on real dissociation, allowing a reused X window to take real X input focus on remap while preserving client focus transfers during live wrapper changes. Dissociation also schedules the deferred native focus clear, so retiring an old popup cannot leave X input focus at `PointerRoot` while a native window owns the seat.

[`protocol/xwayland_popup_classification`](../../tests/harness/checks/protocol/xwayland_popup_classification.sh) exercises real Xwayland transitions under broad tiling and maximize rules. It checks pre-map popup metadata, persistent `MENU` windows remaining managed, mapped type and override-redirect changes, geometry recovery without a new client configure, cleared maximize and fullscreen state, rendered pixels, keyboard input, a passive utility popup keeping typing in its transient parent across remaps, overview card removal, return to managed classification, remapping, and native focus retention after popup withdrawal. Run it as `just check protocol/xwayland_popup_classification`.

## Bounding shapes constrain scene coverage

An X11 bounding shape can contain holes or disconnected rectangles. Xwayland exports the window's rectangular backing buffer without applying that mask to its alpha. Pixels outside the shape can therefore be opaque black, even for an ARGB window with a transparent background. Reading the window with XGetImage does not establish backing-buffer alpha: Xwayland can zero the returned pixels outside its visible region.

`XwaylandShapes` reads `ShapeBounding` on the existing XWM connection and listens for bounding `ShapeNotify` events. Extension discovery and shape replies are asynchronous. Initial queries keep the surface clipped to an empty region until discovery completes; live queries retain the previous mask. A private wakeup event ensures wlroots drains X events that reply polling may have queued. The tracker owns no additional X connection, so it preserves lazy Xwayland shutdown and resets when that server exits.

UmbrielFX stores arbitrary node clips in addons, preserving its scene ABI. A null clip is unrestricted; an empty region hides the node. Region clips intersect existing rectangular clips and constrain drawing, opaque occlusion, damage, output membership, and hit testing. Clipped buffers cannot bypass those regions through direct scanout.

For managed windows, `applySurfaceShape` projects the surface-local X region through the buffer's current source crop and destination size. The same projection applies to isolated capture scenes and overview copies. Masks are copied with close-animation buffers, and blur is confined to the corresponding visible region. Native-resolution X pixels are converted through the drawn scene geometry; output and layout geometry remain logical coordinates. Unmanaged X11 windows refresh their mask after shape, scale, and surface-state changes. Input shapes are handled independently and never serve as drawing masks.

The region primitive is covered by `tests/unit/scene_clip.cpp`. Run `just check protocol/xwayland_shape` for real Xwayland framebuffer, overview, and window-capture transitions at scales 1 and 2. The check preserves painted pixels while shrinking and restoring the shape, so an opaque backing buffer cannot pass through an unrelated alpha fallback. Window capture uses XRGB, so removed widgets become black in that isolated capture while the desktop shows through them on the output.

## Output membership follows real geometry

wlroots emits enter/leave from scene-node visibility with a 10% overlap threshold, and scale-aware clients size and map input from the outputs a surface entered. Two situations cross that threshold:

- Any window whose layout box reaches past its output: a scrolling column scrolled off the shared edge, an unanimated snap move that jumps a node, a workspace slide, a close-fade snapshot.
- Interactive drags legitimately span monitors; every boundary graze enters the neighbor mid-drag. This is accepted as inherent to a drag (the drop's final enter settles it), see below.

Defense:

**Per-output clipped scene roots** (`Output::viewRoot`, `fullscreenRoot`, `pinnedRoot`): each output owns three scene trees carrying a `wlr_scene_tree_set_clip` of that output's layout box, and every workspace tree, fullscreen tree, pinned view and close snapshot hangs under them. `umbrielfx` folds an ancestor tree clip into the visibility walk (`_scene_nodes_in_box` accumulates the clip, `scene_node_update_iterator` intersects `node->visible` with it), so a node that reaches past its own output has no visible region there at all: no rendering, no damage, no membership, no enter event. There is no window position or move sequence that can graze a neighbor, so no per-move mitigation is needed and none exists. The drag tree is deliberately outside those roots, which is what lets a dragged window span both outputs.

A rejected approach, for whoever considers it next: pinning scene nodes to a single output inside `umbrielfx` (filtering `active_outputs` in `update_node_update_outputs`) does suppress the enter/leave churn, but it divorces output membership from rendering. `wlr_scene_output_build_state` asserts on buffers rendered on an output they are not members of (SIGABRT when a pinned window is dragged fully onto another output), and anything keyed on `primary_output` (drop attribution, frame scheduling, dmabuf feedback) silently misbehaves. Membership must stay derived from real scene geometry, which is exactly why the clip changes that geometry instead of filtering its result.

## Native focus fully leaves the X11 domain

wlroots deactivates the focused Xwayland surface by moving X core input focus to `PointerRoot`. Its XWM also publishes a private no-focus window through `_NET_ACTIVE_WINDOW`. That preserves X11 keyboard grabs and popups, but an X11 launcher can continue treating itself as eligible for controller input while a native Wayland game has seat focus. Clearing only `_NET_ACTIVE_WINDOW` is not enough: the X core focus target must leave `PointerRoot` too.

Umbriel handles an Xwayland-to-native keyboard transition in this order:

1. Deliver `wl_keyboard.enter` to the native surface.
2. Arm a one-millisecond Wayland event-loop timer, placing the X11 update on the next dispatch after the server flushes the native enter.
3. Recheck that seat focus is still a non-Xwayland surface.
4. Set X core input focus to `None`, set `_NET_ACTIVE_WINDOW` to `None`, and flush the XWM connection.

The delayed ordering is the standard cross-process handoff: the native enter is delivered before X focus is cleared. Performing the X clear before the native enter can leave the native game unable to receive control. A stale timer cannot clear a newly restored X11 focus because the callback rechecks the seat, and Xwayland-to-Xwayland refreshes never schedule it.

Using `None` means X11 keyboard grabs are unavailable while a native client is focused, which is why wlroots normally prefers `PointerRoot`. Umbriel limits that tradeoff to a real Xwayland-to-native transfer and lets wlroots restore normal X focus when the seat returns to Xwayland.

## NoInput windows preserve the typing target

An X11 window with `WM_HINTS.input = False` and no `WM_TAKE_FOCUS` protocol uses ICCCM's NoInput model. Steam's on-screen keyboard advertises these hints as a utility window. Mapping it or interacting with its pointer surface must not activate it or replace the seat's keyboard owner.

`View::acceptsKeyboardFocus()` reads wlroots' ICCCM input-model classification. The focus manager rejects NoInput targets before changing activation, window recency, or workspace focus. The seat-focus mechanism checks the same eligibility, and replacement focus skips NoInput windows. If a focused client changes its hints to NoInput, Umbriel clears its keyboard focus and selects an eligible replacement. Pointer hit testing and delivery remain available.

This follows window hints rather than Steam titles or application identities. An `Input=False` client that implements `WM_TAKE_FOCUS` has a different input model and is not classified as NoInput.

## Native selections are reannounced on X11 focus return

wlroots publishes Wayland clipboard ownership changes immediately, including while a native client has keyboard focus. It rejects X11 selection reads until an Xwayland surface is focused. Wine reads targets eagerly on ownership notifications, so a denied background read can leave its clipboard cache empty even after the user returns to the game.

On a non-Xwayland or empty-seat transition into Xwayland, Umbriel arms a one-millisecond selection timer. The callback rechecks the current seat focus and live XWM, then rebinds the same seat through `wlr_xwayland_set_seat()`. wlroots reannounces native clipboard and primary selections, allowing eager readers to populate their caches after activation. Its X11-owned sources are left intact, and its background-read restriction remains unchanged.

The timer is persistent, so focus changes do not allocate event sources. Xwayland-to-Xwayland transfers do not schedule a refresh, and a stale timer cannot publish selections after the seat has left Xwayland.

## Xwayland theme cursors use output-native images

Xwayland submits X11 cursors as scale-1 Wayland surfaces. wlroots correctly scales an arbitrary client surface to the output, but it cannot recover the semantic Xcursor name needed to load the theme's image for that output scale. The result is a correctly sized but visibly blurred cursor on scaled outputs.

Umbriel promotes a cursor surface only when all of these conditions hold:

- The surface belongs to Umbriel's Xwayland Wayland client. Native Wayland cursor surfaces are never candidates.
- The surface uses buffer scale 1, the normal transform, and no viewport. Its dimensions, hotspot, and ARGB8888 pixels exactly match an image in the configured theme at scale 1. RGB values under fully transparent pixels are ignored because those channels are undefined.
- Any animation frame may supply the match, and the previously matched semantic name wins when aliases share an image.

The compositor then gives wlroots the matched name. wlroots loads the theme image for each output scale, so the cursor stays sharp without changing the X11 application's cursor size. Unmatched images remain ordinary client-owned surfaces, preserving custom game cursors and application animation. Cursor commits and cursor-theme reloads re-run the classification in both directions.

The incoming Xwayland buffer is copied during the surface's `client_commit` signal. At that point the source buffer is still readable; wlroots may reuse an existing uploaded texture and release that source before the later `commit` signal. Once a surface is promoted or otherwise detached by cursor hiding or an Umbriel cursor override, Umbriel also completes its frame callback so Xwayland can submit the next cursor image. Visible raw surfaces retain wlroots' normal output pacing.

This deliberately stays inside the compositor. wlroots preserves raw client cursor surfaces and already provides the named, output-scale-aware path used after a match. Global Xwayland scaling requires a patched X server. Raising `XCURSOR_SIZE` for the whole session also changes applications that already account for scale. Neither approach can safely distinguish a theme cursor from a custom one, and application-specific rules would only move that ambiguity into configuration.

## X11 games can retain stale input after a windowed resize round trip

A fake-fullscreen game (borderless window at output size) that receives a compositor-imposed windowed size, then returns to fullscreen, can keep a stale mouse mapping: X geometry, stacking, focus, and event delivery all recover, but hover and clicks die outside the transient size. This is upstream Wine behavior; the boundary of the dead zone always equals whatever transient size the compositor sent.

Umbriel therefore avoids incidental windowed resize round trips while keeping deliberate fullscreen exits authoritative:

1. **Float toggle** (`View::setFloating(true)`) keeps the current size when floating a fullscreen window (no transient resize), and records `m_refullscreenOnTile`.
2. **Re-tile** (`View::setFloating(false)`) restores fullscreen before the layout attach when that flag is set, so arrange sizes the column to the full output the client already has. A client that chose windowed mode itself clears the flag (`setFullscreen(false)` from any other path) and re-tiles as a regular column.
3. **Fullscreen exit** (`View::setFullscreen(false)`): a tiled view clears fullscreen and sets its restored column size in the same client configure. This applies equally to native Wayland and X11 views. There is no timer and no size-0x0 probe, so an X11 client that keeps its fullscreen-sized buffer cannot make the compositor undo the action. Client requests that arrive later still use the normal fullscreen request path.

Real migrations (dropping a window on a differently scaled output) still resize by necessity and can still break fragile games. A deliberate fullscreen exit followed later by fullscreen entry also necessarily crosses a windowed size. Those are upstream constraints, not transitions the compositor can mask without ignoring the requested state.

## Verifying changes here

The clip primitive itself is covered by [`tests/unit/scene_clip.cpp`](../../tests/unit/scene_clip.cpp): two headless outputs, a buffer straddling the shared edge, and assertions on the enter/leave stream, the primary output and hit testing as the clip is set, nested, and cleared.

Compositor-level containment is covered by [`tests/harness/checks/output/two_output_containment.sh`](../../tests/harness/checks/output/two_output_containment.sh), which declares `# harness: outputs=2` so the harness boots it a two-output instance, and compares real framebuffers while a strip overflows the shared edge. Run it as `just check output/two_output_containment`.

The shared fullscreen-exit ordering is covered by `just check layout/fullscreen_exit_configure`: the first windowed configure must already contain the restored tile size. That check does not exercise the X11 resize path or multi-output X coordinate spaces, so the X11 path still needs a running session with Steam or another X11 game. A fullscreen exit must start the windowed resize immediately and remain windowed after the animation settles. For the protected float and re-tile round trip, the signature to reject is a ConfigureNotify pair through a non-fullscreen size; a passive `StructureNotifyMask` monitor on `DISPLAY=:0` shows it directly.

The X11-to-native focus handoff is covered by [`tests/harness/checks/focus/xwayland_native_handoff.sh`](../../tests/harness/checks/focus/xwayland_native_handoff.sh). It starts the harness's private Xwayland server, transfers focus from a real X11 window to a native client, and requires X core focus and `_NET_ACTIVE_WINDOW` to both become `None`. It also verifies that the native client receives keys, the old X11 client does not, and returning to X11 restores both focus channels and key delivery. Run it as `just check focus/xwayland_native_handoff`.

Selection focus return is covered by [`tests/harness/checks/protocol/xwayland_selection.sh`](../../tests/harness/checks/protocol/xwayland_selection.sh). Its X11 consumer reads targets and text on ownership notifications, then reports its cache without a paste-time retry. It checks repeated native updates, fullscreen, primary selection, background-read denial, preserved X11-owned sources, and clears. Run it as `just check protocol/xwayland_selection`.

NoInput focus is covered by [`tests/harness/checks/focus/xwayland_no_input.sh`](../../tests/harness/checks/focus/xwayland_no_input.sh). It reproduces Steam's utility-window hints and requires native keyboard ownership and real XTest typing to survive mapping, hovering, clicking, explicit focus requests, and closing the overlay. An ordinary X11 window still receives focus, and closing it restores the native typing target while the NoInput overlay remains open. Run it as `just check focus/xwayland_no_input`.

The same check temporarily changes the mapped overlay to accept input, verifies X11 key delivery, then changes its hints back to NoInput and requires a new native keyboard-enter event and restored typing.

Theme cursor identity is covered by [`tests/unit/xcursor_matcher.cpp`](../../tests/unit/xcursor_matcher.cpp), including padded rows, transparent pixels, exact hotspots, aliases, and animation frames. The live Xwayland path is covered by [`tests/harness/checks/input/xwayland_cursor_promotion.sh`](../../tests/harness/checks/input/xwayland_cursor_promotion.sh). It uses a deterministic two-size Xcursor theme on a scale-2 headless output and checks promotion, custom-image and hotspot rejection, reload reclassification, native Wayland exclusion, and focus re-entry. Run it as `just check input/xwayland_cursor_promotion`.
