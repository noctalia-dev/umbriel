# Overview rendering

This note records rendering and interaction details that are too specific for
the main configuration guide but remain part of Umbriel's observable behavior.

## Live content

Overview cards display live window content. The real workspace windows are
hidden while the overview is open. Wheel steps move one workspace at a time,
while touchpad navigation drags the previews and selects on release.
Configured focus actions retain their layout-specific behavior.

Transparent windows keep their window-rule blur throughout the zoom
transition.

## Touchpad navigation

Two-finger scrolling and three-finger swipes reach the same `OverviewNavigation`
state: deltas in content direction, one locked axis after 16 units of travel,
rubber-banded 0.15 of a workspace or viewport past either end, and a release
position projected 120 ms along the recent velocity. Neither stream commits a
workspace before its release.

The two streams carry different travel distances because libinput reports
swipes as pointer-accelerated motion and finger scrolling as raw scroll units.
One workspace is 300 units of swipe, matching the workspace switch outside the
overview, and 500 units of finger scrolling; one viewport of strip panning is
1200 and 500 respectively. Distances scale with the settled preview zoom rather
than the zoom in flight, so a gesture that starts during the opening animation
travels the same distance as one that starts after it.

The filmstrip is one `AnimatedValue` per output, in workspace rows. Every
source moves it the same way through `Overview::animateRow`, which uses
`[animation.overview] workspace_curve`: a spring curve settles from the current
position through `AnimatedValue::settleSpring`, carrying the release velocity
scaled by the rubber-band derivative at the release point; any other curve runs
over `duration_ms` from rest. A gesture in flight snaps the value each frame,
which also stops a settle still running on that output.

## Animation ownership

Cards use separate scene buffers because the overview scales and clips each
window into a workspace preview. They do not own a second window animation
state. Every `View` remains the authority for its currently presented position,
size, and opacity, including for a hidden workspace while the overview is open.
The overview projects that presented box through `Overview::previewBox` and its
zoom.
Card borders consume the same presented opacity as their window content, so
map fades and window-rule opacity cannot reveal a ring ahead of its surface.

Only overview-specific motion lives in `Overview`: opening and closing zoom,
filmstrip scrolling, card dragging, and drop hints. Layout movement, resize,
and fade transitions continue to advance in `View`, so the overview and the
normal workspace cannot settle through different paths.

Selecting a card focuses it before the closing zoom starts. The overview still
withholds keyboard input until teardown, while a scrolling layout can begin
revealing the selected column on the same frame and animation timeline as the
zoom. The close therefore lands directly on the selected column instead of
starting a second movement afterwards.

Configured keybinds continue to dispatch during the closing zoom. A later focus
or workspace selection replaces the card that initiated the close as the
landing target. Workspace retargets use a separate animation value, so repeated
navigation cannot extend the zoom deadline.

Unmap is the one transition that cannot remain live because the client buffer
may disappear immediately. Before removing an unmapped card, the overview
freezes its already-scaled buffers and borders into a scene snapshot. That tree
uses the same `Server::CloseSnapshot` animation owner, easing, half-duration,
and starting buffer opacity as a close on the normal workspace. Overview owns
only the projection into card coordinates, not a separate close timeline.

## Decoration and clipping

Cards carry the same inner border, outer border, and corner radius as their
windows. These values scale with the card. Every surface of a card rounds
against the card's content box, the rule live windows use, so a client that
draws its corners from a subsurface keeps them rounded in the thumbnail.

Each output's overview tree carries a `wlr_scene_tree_set_clip` of that output's
logical bounds, the same primitive windows use. It is the only clip a card is
subject to: previews step along the output's workspace axis and a strip pushes
its cards past the preview across that axis on purpose, so cards, border rings,
and workspace backgrounds are contained by that one output clip and none of them
trims its own geometry. The dragged card is reparented out to the unclipped
overview root so it can span outputs, exactly as a dragged window does.

Each workspace has a rounded background behind its cards. The configured alpha
controls whether this is a light tint, a translucent panel, or an opaque fill.

With `overview.workspace_wallpaper`, one passive scene buffer per preview and
per mirrored surface draws over that fill. The source is the output's background-
and bottom-layer trees, walked in place so the copies keep their render order,
and each surface's output-local box maps through the same preview origin and zoom
the cards use. Every preview clips its own mirrors, so a partially anchored
surface cannot reach into the gap between previews.

The real bottom layer is disabled for as long as the overview is open, exactly
as the window trees are: a bottom-layer surface appears once per workspace
instead of twice at two scales, and at zoom 1 the previews reproduce the output
pixel for pixel, so neither the opening nor the teardown swap is visible. Those
copies are then the only place their surfaces are sampled, so each one carries
the presentation feedback, release points, and frame callbacks that pace its
client. The background layer stays enabled: it is the blur source and what shows
around the filmstrip. An output where no client maps either layer shows the flat
fill.

A dedicated scene root between the layer-shell background and bottom layers
carries each output's wallpaper blur node. This placement blurs the background
layer while bottom-layer surfaces render afterward and remain sharp. The node's
alpha and strength fade with zoom progress. When `[appearance.blur] optimized`
is enabled, it samples the optimized background buffer. The node is absent when
appearance blur or `overview.background_blur` is disabled.

No window holds the seat while the overview is open, so exactly one card can
wear `colors.border.focused`: the live target, meaning the focused view of
the active workspace on the output under the cursor. That is the window a focus
or close action resolves to through `preferredOutput()`, so the strong border
also identifies the current output. Every other workspace marks its own focused
view with a blend of `colors.border.focused` into `colors.border.unfocused`,
showing where that workspace would land without claiming focus. An empty current
workspace leaves no strong border, which is also when those actions have no
target.

The live target is resolved per layout pass. `Overview::handleMotion` repaints
when the pointer changes output, which covers both hand motion and the cursor
warp an output-changing keybind performs, and `onFocusChanged` covers the rest.

Closing the focused window reassigns focus to its nearest predecessor while the
overview stays open, or to the next neighbor when there is no predecessor. The
markers move with it.

## Dragging

A dragged card renders at 0.75 opacity (`View::kDragOpacity`). This multiplier
combines with the client's own surface alpha rather than replacing it, which
keeps the insertion preview visible through the card.

The scrolling layout previews insertion beside the actual column edges. For an
overflowing strip, prepend and append previews remain visible at the output
edges. The dwindle layout previews the direction of the split before the card
is dropped.

## Verification

The relevant checks are:

- [`tests/harness/checks/310_overview_wheel.sh`](../../tests/harness/checks/310_overview_wheel.sh)
  for overview interaction and workspace navigation.
- [`tests/harness/checks/346_overview_keybind_actions.sh`](../../tests/harness/checks/346_overview_keybind_actions.sh)
  for configured directional actions and fallback arrow navigation.
- [`tests/harness/checks/460_external_drag.sh`](../../tests/harness/checks/460_external_drag.sh)
  for client drag ownership during overview activation.
- [`tests/harness/checks/430_drag_opacity.sh`](../../tests/harness/checks/430_drag_opacity.sh)
  for composed drag opacity.
- [`tests/harness/checks/450_drag_left_hint.sh`](../../tests/harness/checks/450_drag_left_hint.sh)
  for the visible prepend target on an overflowing scrolling strip.
- [`tests/harness/checks/320_overview_refocus.sh`](../../tests/harness/checks/320_overview_refocus.sh)
  for adjacent focus reassignment when the focused window closes in the
  overview.
- [`tests/harness/checks/330_overview_close_fade.sh`](../../tests/harness/checks/330_overview_close_fade.sh)
  for a card remaining visible after unmap and disappearing when the shared
  close snapshot settles.
- [`tests/harness/checks/340_overview_focus_motion.sh`](../../tests/harness/checks/340_overview_focus_motion.sh)
  for selected-column focus and reveal beginning during the closing zoom.
- [`tests/harness/checks/361_overview_focus_marker.sh`](../../tests/harness/checks/361_overview_focus_marker.sh)
  for one live marker across two outputs, following both an output-changing
  keybind and plain pointer motion.
- [`tests/harness/checks/350_overview_horizontal_overflow.sh`](../../tests/harness/checks/350_overview_horizontal_overflow.sh)
  for cards extending past the scaled workspace preview on either axis while
  staying inside the output.
- [`tests/harness/checks/360_slide_viewport_clips.sh`](../../tests/harness/checks/360_slide_viewport_clips.sh)
  for a sliding workspace clipping its overhanging content to the viewport it
  travels with.
- [`tests/harness/checks/650_two_output_containment.sh`](../../tests/harness/checks/650_two_output_containment.sh)
  for cards staying off a neighbouring output, overview included.
- [`tests/unit/presented_crop.cpp`](../../tests/unit/presented_crop.cpp) for the
  presented-crop math shared with window presentation.
