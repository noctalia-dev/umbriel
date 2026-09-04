# Overview rendering

This note records rendering and interaction details that are too specific for
the main configuration guide but remain part of Umbriel's observable behavior.

## Live content

Overview cards display live window content. The real workspace windows are
hidden while the overview is open, so wheel steps, fallback vertical arrow
keys, and 3-finger swipes move one workspace at a time instead of sliding the
live workspace. Configured vertical focus actions retain their layout-specific
behavior, including stacked-card traversal in horizontal scrolling workspaces.

Transparent windows keep their window-rule blur throughout the zoom
transition.

## Animation ownership

Cards use separate scene buffers because the overview scales and clips each
window into a workspace row. They do not own a second window animation state.
Every `View` remains the authority for its currently presented position, size,
and opacity, including for a hidden workspace while the overview is open. The
overview projects that presented box through its row and zoom transform.
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
landing target. Workspace-row retargets use a separate animation value, so
repeated navigation cannot extend the zoom deadline.

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

Each output's overview tree carries a `wlr_scene_tree_set_clip` of that
output's logical bounds, the same primitive windows use. A workspace row that
pushes a card past an output edge is scissored there: cards, border rings, and
workspace backgrounds are all contained by that one clip, and none of them
trims its own geometry. The dragged card is reparented out to the
unclipped overview root so it can span outputs, exactly as a dragged window
does.

Each workspace has a rounded background behind its cards. The configured alpha
controls whether this is a light tint, a translucent panel, or an opaque fill.

With `overview.workspace_wallpaper`, one passive scene buffer per row and per
mirrored surface draws over that fill. The source is the output's background-
and bottom-layer trees, walked in place so the copies keep their render order,
and each surface's output-local box maps through the same row and zoom transform
the cards use. Every row clips to its own bounds, so a partially anchored
surface cannot reach into the gap between rows.

The real bottom layer is disabled for as long as the overview is open, exactly
as the window trees are: a bottom-layer surface appears once per workspace
instead of twice at two scales, and at zoom 1 the rows reproduce the output
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
also identifies the current output. Every other row marks its own focused view
with a blend of `colors.border.focused` into `colors.border.unfocused`, showing
where the row would land without claiming focus. An empty current workspace
leaves no strong border, which is also when those actions have no target.

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
  for cards extending past the scaled workspace background while staying inside
  the output.
- [`tests/harness/checks/360_vertical_viewport_clips.sh`](../../tests/harness/checks/360_vertical_viewport_clips.sh)
  for a vertical strip presented as one live viewport per overview row.
- [`tests/harness/checks/650_two_output_containment.sh`](../../tests/harness/checks/650_two_output_containment.sh)
  for cards staying off a neighbouring output, overview included.
- [`tests/unit/presented_crop.cpp`](../../tests/unit/presented_crop.cpp) for the
  presented-crop math shared with window presentation.
