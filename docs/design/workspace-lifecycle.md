# Workspace lifecycle

This note records the workspace state transitions behind the shorter user guide
in [`outputs.md`](../../user/outputs.md).

## Dynamic inventory

A dynamic output maintains numbered workspaces with these invariants:

- It always has a trailing empty workspace.
- With `workspaces.empty_above` enabled, it also has a distinct leading empty
  workspace, including before the first view maps.
- Outside a workspace slide or overview session, an occupied sentinel causes
  Umbriel to add a new empty workspace at that edge.
- Other empty inactive workspaces are removed, except that the count never
  drops below the output's `min_workspaces`. Pruning runs from the end, so the
  surviving empties are the lowest-numbered ones.
- Remaining workspaces are renamed and reindexed from `1` in their current
  order.
- Workspace layout rules are resolved again after renumbering.

Dynamic reconciliation waits while a workspace slide or the overview is active.
This prevents the workspace list from changing underneath those interactions.

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

Mapping a window or activating a workspace can replace the scene under a
stationary pointer without crossing a window border. With `follows_mouse`
enabled, Umbriel does not override the mapping or workspace focus immediately.
The focus transition instead invalidates the previous hover decision once. The
next eligible pointer motion can therefore select a newly revealed view even
when both the old and new pointer coordinates fall inside it. After that one
refresh, hover returns to geometric border-crossing detection so scrolling
animations cannot cascade focus through windows moving beneath the pointer.

This distinction matters when a second window maps away from the cursor and
when returning to a workspace whose remembered focused window is elsewhere.
In both cases, a small motion inside the window under the pointer is sufficient;
the pointer does not need to leave and re-enter its border.

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

A number or ordered name list defines an exact static inventory. During a
configuration reload, Umbriel preserves workspace identity in two passes:

1. Match existing workspaces to the new inventory by name.
2. Match any remaining entries by position.

Umbriel creates entries that have no match. When an old workspace is removed,
its windows move to the surviving workspace at the same position, or to the
last workspace when that position no longer exists. If the active workspace is
removed, that destination becomes active.

Empty static workspaces remain in the inventory.

## Switching inventory type

Switching from a static inventory to dynamic workspaces keeps every populated
workspace and the active workspace. Other empty workspaces are removed. The
survivors are renumbered, and Umbriel appends empty workspaces until the count
reaches `min_workspaces` and the inventory ends in an empty workspace. The
optional leading empty workspace is restored on top of that.
Switching to a static inventory follows the normal name-first, position-second
matching process.

## Workspace layout rules

Layout settings resolve in this order:

1. Base `[layout]` settings.
2. A matching global `[[workspace]]` rule.
3. A matching output-specific `[[workspace]]` rule.

Dynamic rules are resolved against the workspace's current number after any
inventory change.

## Verification

Configuration resolution and change classification are covered by
[`tests/unit/config_resolve.cpp`](../../tests/unit/config_resolve.cpp) and
[`tests/unit/config_change.cpp`](../../tests/unit/config_change.cpp). Live
workspace selection is exercised by
[`tests/harness/checks/210_workspace_selectors.sh`](../../tests/harness/checks/210_workspace_selectors.sh).
Leading and trailing dynamic sentinels, including renumbering after workspace
movement, are covered by
[`tests/harness/checks/215_empty_above.sh`](../../tests/harness/checks/215_empty_above.sh).
The per-output dynamic floor is covered by
[`tests/harness/checks/216_min_workspaces.sh`](../../tests/harness/checks/216_min_workspaces.sh).
Pointer isolation during a wheel-triggered workspace transition is covered by
[`tests/harness/checks/220_workspace_transition_focus.sh`](../../tests/harness/checks/220_workspace_transition_focus.sh).
Hover focus after a window maps under the pointer and after returning to a
workspace is covered by
[`tests/harness/checks/511_spawn_hover_focus.sh`](../../tests/harness/checks/511_spawn_hover_focus.sh)
and
[`tests/harness/checks/512_workspace_return_hover_focus.sh`](../../tests/harness/checks/512_workspace_return_hover_focus.sh).
Scrolling reveal animations are kept from cascading hover focus by
[`tests/harness/checks/513_scrolling_hover_focus_stability.sh`](../../tests/harness/checks/513_scrolling_hover_focus_stability.sh).
Modifier-wheel switching and the resulting keyboard-focus handoff through an
input-method keyboard grab are covered by
[`tests/harness/checks/520_input_method_wheel.sh`](../../tests/harness/checks/520_input_method_wheel.sh).
Client-cursor refresh after a short data-device drag is covered by
[`tests/harness/checks/460_external_drag.sh`](../../tests/harness/checks/460_external_drag.sh).
Keyboard-focus replay after a logical focus change during a drag is covered by
[`tests/harness/checks/470_data_drag_focus.sh`](../../tests/harness/checks/470_data_drag_focus.sh).
Drop-target hover focus and the subsequent keyboard-focus handoff are covered by
[`tests/harness/checks/471_data_drag_hover_focus.sh`](../../tests/harness/checks/471_data_drag_hover_focus.sh).
