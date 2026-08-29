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
- Other empty inactive workspaces are removed.
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

## Data-device drag focus

Wayland data-device drags install a keyboard grab that deliberately suppresses
keyboard enters until the drag finishes. Logical focus can still move during
the grab, for example when focus-follows-mouse crosses onto another output.
Umbriel updates activation and border state immediately, then replays that
already selected view into the default seat keyboard grab when the drag is
destroyed. It does not select the drop target merely because a drag ended.

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
survivors are renumbered, and Umbriel restores the trailing empty workspace plus
the optional leading empty workspace.
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
Pointer isolation during a wheel-triggered workspace transition is covered by
[`tests/harness/checks/220_workspace_transition_focus.sh`](../../tests/harness/checks/220_workspace_transition_focus.sh).
Modifier-wheel switching and the resulting keyboard-focus handoff through an
input-method keyboard grab are covered by
[`tests/harness/checks/520_input_method_wheel.sh`](../../tests/harness/checks/520_input_method_wheel.sh).
Keyboard-focus restoration after a data-device drag is covered by
[`tests/harness/checks/470_data_drag_focus.sh`](../../tests/harness/checks/470_data_drag_focus.sh).
