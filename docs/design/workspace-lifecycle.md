# Workspace lifecycle

This note records the workspace state transitions behind the shorter user guide
in [`outputs.md`](../user/outputs.md).

## Dynamic inventory

A dynamic output contains anonymous positional workspaces and may also contain
persistent named workspaces declared by `[[workspace]]`. Both use the same
runtime workspace object and stable identity. Its `named` flag controls whether
its label is explicit or generated from its current position.

The inventory maintains these invariants:

- Below the 64-workspace limit, it has a trailing empty anonymous workspace. A
  named empty workspace cannot serve as this sentinel.
- With `workspaces.empty_above` enabled, it also has a distinct leading empty
  anonymous workspace while capacity permits, including before the first view
  maps.
- Outside a workspace slide, an occupied sentinel causes Umbriel to add a new
  empty workspace at that edge when capacity remains. At the limit, the
  occupied edge persists until another slot becomes available.
- Other empty inactive anonymous workspaces are removed, except that the count
  never drops below the output's `min_workspaces`. Pruning runs from the end, so
  the surviving anonymous empties are the lowest-positioned ones.
- Named workspaces remain even when empty.
- Anonymous workspaces are renamed and all workspaces are reindexed from `1` in
  their current order. Named workspaces keep their explicit names.
- Workspace layout rules are resolved again after renumbering.

Dynamic reconciliation waits while a workspace slide is active. During the
overview it proceeds immediately and notifies the overview so its inventory can
be rebuilt.

### Reconciling configured names

For each dynamic output, the desired named members are the unscoped
`[[workspace]]` name entries plus entries whose `output` matches it. Workspace
names are case-sensitive and output-local. Output selectors retain their
ordinary case-insensitive identity matching. An unscoped name therefore
produces an independent workspace on every dynamic output.

On initial construction, named members follow declaration order near the top,
after the optional leading sentinel. On reload, Umbriel first preserves every
existing desired member by exact name. Below the output limit, newly added names
enter in declaration order at the same top insertion point. At the limit, a
name can instead claim an eligible empty anonymous workspace in place.
Reordering declarations alone does not move existing workspaces.

When a name is no longer desired on an output, Umbriel clears its `named` flag
and assigns the anonymous label for its current position. The workspace keeps
its identity and windows, then participates in normal dynamic reconciliation.
It survives while populated or active and may be pruned once empty and
inactive.

At the 64-workspace runtime limit, a missing configured name reuses an empty
anonymous workspace only when that workspace is not a leading or trailing
sentinel. Otherwise the declaration remains pending and reconciliation retries
after later workspace activity frees an ordinary anonymous entry.

## Switching interaction

A workspace slide keeps the outgoing workspace's scene enabled until the
animation finishes. Those views are render-only once their workspace becomes
inactive: pointer hit-testing must ignore them even though their buffers remain
visible. Otherwise a click during the slide can focus an outgoing window,
reactivate the workspace the user just left, and return keyboard or text-input
focus to that client.

The incoming active workspace remains interactive throughout the transition.
Pinned windows and scratchpad windows do not inherit this inactive-workspace
restriction.

## Pointer focus after scene changes

Mapping a window, activating a workspace, or running a layout command can
replace the scene under a stationary pointer without crossing a window border.

Clients learn the pointer position only from `wl_pointer.enter` and `motion`,
and a button event carries no coordinates. Umbriel therefore hit-tests again
before delivering every press and sends a motion when the surface under the
cursor has moved. It also re-resolves the pointer after any output frame whose
scene changed, once animations, gestures, and the overview on that output have
settled, so hover state follows content that moved under a still cursor. That
refresh never changes keyboard focus, and it pauses while a button is held so an
implicit grab keeps the coordinate space of its press.

With `follows_mouse` enabled, Umbriel does not override the established focus
immediately. The focus transition or successful command instead invalidates the
previous hover decision once. The next eligible pointer motion can therefore
select a newly revealed view even when both the old and new pointer coordinates
fall inside it. After that one refresh, hover returns to geometric
border-crossing detection so scrolling animations cannot cascade focus through
windows moving beneath the pointer.

The hover decision compares against seat-global activation, not a workspace's
remembered focus. A pinned window can retain seat focus while following the
output away from its owning workspace; leaving it must still activate the view
under the pointer even when the active workspace already remembers that view.

This distinction matters when a second window maps away from the cursor and
when returning to a workspace whose remembered focused window is elsewhere. It
also covers explicit strip scrolling, column resizing or reordering, and leaving
fullscreen when it reveals a different window. Fullscreen exits behave the same
whether Umbriel runs a configured action or a client such as a browser or media
player leaves video fullscreen through XDG shell. In each case, a small motion
inside the window under the pointer is sufficient; the pointer does not need to
leave and re-enter its border.

Normal tiled close replacement preserves geometric proximity between columns.
When it must cross into a surviving column with several rows, it resolves those
rows through the global focus history. Visual row order describes placement,
not which member the user focused before opening the window that just closed.

Closing a focused Dwindle or master tile is a bounded exception. Umbriel records
the pointer position only when the closing view owns keyboard focus, is visibly
beneath the pointer, and the position lies inside its current presented box. It chooses the
normal layout replacement before detaching the view, then flushes the new layout
and checks its final tiled target boxes once. A survivor that inherits the
recorded position receives pointer-hover focus; otherwise the normal replacement
remains focused. Scene hit-testing instead of cached seat pointer focus keeps
consecutive closes correct without pointer motion. Reading final layout geometry
avoids treating every view that moves through the pointer during an animation as
another hover transition.

## Data-device drag focus

Wayland data-device drags install pointer and keyboard grabs. Normal hover
focus is suspended while the drag owns pointer motion, and keyboard enters are
suppressed until the drag finishes. When the initiating button release destroys
the grab, Umbriel reruns pointer processing at the unchanged cursor position.

With `follows_mouse` enabled, that refresh selects the window under the pointer
when it is not already focused. Activation, border state, and keyboard focus
then follow the drag target without requiring another border crossing. With
`follows_mouse` disabled, the refresh restores normal pointer delivery and the
client cursor while retaining the existing window focus.

## Static inventory

A number defines exact anonymous positions. An ordered string list defines an
exact named inventory. During a configuration reload, Umbriel preserves
workspace identity in two passes:

1. Match named entries in the new inventory by name.
2. Match any remaining entries by position.

Umbriel creates entries that have no match. When an old workspace is removed,
its windows move to the surviving workspace at the same position, or to the
last workspace when that position no longer exists. If the active workspace is
removed, that destination becomes active.

Empty static workspaces remain in the inventory. Members from a count are
anonymous and use their one-based position as a generated label. Members from
a string list are named. A `[[workspace]]` name entry changes a static member
only where its name and optional output scope match; it does not extend the
configured inventory. An unscoped rule may therefore affect matching named
static outputs while also materializing its name on dynamic outputs.

## Switching inventory type

Switching from a static inventory to dynamic workspaces keeps every populated
workspace and the active workspace. Entries whose explicit names remain
declared for the dynamic output keep those names; other members are anonymous.
Other empty anonymous workspaces are removed. The survivors are renumbered,
matching named entries are materialized, and Umbriel appends empty anonymous
workspaces until the count reaches `min_workspaces` and, while capacity permits,
the inventory ends in an empty sentinel. The optional leading empty workspace
is restored when another slot remains. At 64 occupied survivors, missing edge
sentinels wait until a slot becomes available. Switching to a static name list
uses name-first, position-second matching; switching to a count uses positions.

## Workspace layout rules

Layout settings resolve in this order:

1. Base `[layout]` settings.
2. The matching output's `layout.scrolling.default_extent_fraction`.
3. A matching global `[[workspace]]` rule.
4. A matching output-specific `[[workspace]]` rule.

A name rule materializes its persistent member on a matching dynamic output and
then follows that identity. An index rule never creates a workspace and is
resolved against the current position after every inventory change. In TOML,
`name = "2"` is an explicit numeric-looking name and remains distinct from
`index = 2`.

Workspace action parsing preserves the same distinction as a typed reference.
Bare digits produce a position, ordinary text produces a name, and double
quotes force a name for numeric-looking values. Resolution never retries one
type as the other.

## Verification

Configuration resolution and change classification are covered by
[`tests/unit/config_resolve.cpp`](../../tests/unit/config_resolve.cpp) and
[`tests/unit/config_change.cpp`](../../tests/unit/config_change.cpp). Live
workspace selection is exercised by
[`tests/harness/checks/210_workspace_selectors.sh`](../../tests/harness/checks/210_workspace_selectors.sh),
including a bare position on the pointer-preferred output versus an exact
numeric name selected with quotes.
Leading and trailing dynamic sentinels, including renumbering after workspace
movement, are covered by
[`tests/harness/checks/215_empty_above.sh`](../../tests/harness/checks/215_empty_above.sh).
The per-output dynamic floor is covered by
[`tests/harness/checks/216_min_workspaces.sh`](../../tests/harness/checks/216_min_workspaces.sh).
Persistent names mixed with dynamic anonymous workspaces, including reload
addition, removal, ordering, sentinels, and numeric-looking names, are covered
by
[`tests/harness/checks/217_dynamic_named_workspaces.sh`](../../tests/harness/checks/217_dynamic_named_workspaces.sh).
Pending name materialization and sentinel preservation at the runtime limit are
covered by
[`tests/harness/checks/217_dynamic_named_capacity.sh`](../../tests/harness/checks/217_dynamic_named_capacity.sh).
Pointer isolation during a wheel-triggered workspace transition is covered by
[`tests/harness/checks/220_workspace_transition_focus.sh`](../../tests/harness/checks/220_workspace_transition_focus.sh).
Hover focus after a window maps under the pointer and after returning to a
workspace is covered by
[`tests/harness/checks/511_spawn_hover_focus.sh`](../../tests/harness/checks/511_spawn_hover_focus.sh)
and
[`tests/harness/checks/512_workspace_return_hover_focus.sh`](../../tests/harness/checks/512_workspace_return_hover_focus.sh).
The handoff from a pinned window owned by another workspace is covered by
[`tests/harness/checks/512_pinned_workspace_hover_focus.sh`](../../tests/harness/checks/512_pinned_workspace_hover_focus.sh).
Scrolling close restoration within a stacked neighboring column is covered by
[`tests/harness/checks/225_scrolling_close_focus_memory.sh`](../../tests/harness/checks/225_scrolling_close_focus_memory.sh).
Scrolling reveal animations are kept from cascading hover focus by
[`tests/harness/checks/513_scrolling_hover_focus_stability.sh`](../../tests/harness/checks/513_scrolling_hover_focus_stability.sh).
Command-driven strip scrolling, resizing, column movement, and fullscreen exit
are covered by the `518_*_hover_focus.sh` checks.
Client-requested XDG fullscreen exit is covered by
[`tests/harness/checks/519_client_fullscreen_exit_hover_focus.sh`](../../tests/harness/checks/519_client_fullscreen_exit_hover_focus.sh).
Modifier-wheel switching and the resulting keyboard-focus handoff through an
input-method keyboard grab are covered by
[`tests/harness/checks/520_input_method_wheel.sh`](../../tests/harness/checks/520_input_method_wheel.sh).
Client-cursor refresh after a short data-device drag is covered by
[`tests/harness/checks/460_external_drag.sh`](../../tests/harness/checks/460_external_drag.sh).
Keyboard-focus replay after a logical focus change during a drag is covered by
[`tests/harness/checks/470_data_drag_focus.sh`](../../tests/harness/checks/470_data_drag_focus.sh).
Drop-target hover focus and the subsequent keyboard-focus handoff are covered by
[`tests/harness/checks/471_data_drag_hover_focus.sh`](../../tests/harness/checks/471_data_drag_hover_focus.sh).
