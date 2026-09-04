# Maintainer design notes

The main documentation explains how to configure and use Umbriel. The notes in
this directory preserve detailed contracts and implementation constraints for
people changing the compositor.

Start with the user guide when a detail affects configuration or normal use.
Use a design note when the detail explains state transitions, subsystem
boundaries, or regression-sensitive behavior.

- [Configuration reload](configuration-reload.md)
- [Workspace lifecycle](workspace-lifecycle.md)
- [Overview rendering](overview-rendering.md)
- [Border rendering](border-rendering.md)
- [Xwayland input stability](xwayland-input-stability.md)
- [Client buffer constraints](client-buffer-constraints.md)
- [Scene helper ownership](scene-helper-ownership.md)

## Pointer drag completion

A client data-device drag temporarily replaces normal pointer delivery with a
seat grab. When the initiating button release ends that grab,
`Cursor::processButton` reruns pointer motion at the unchanged layout position.
This is required even when the pointer did not move: clients use the fresh
surface-local input to recalculate hover state and restore their cursor image.
When `follows_mouse` is enabled, the same refresh selects a different window
under the pointer and restores keyboard focus there after the drag grab ends.
The short-drag cursor refresh is covered by
[`460_external_drag.sh`](../../tests/harness/checks/460_external_drag.sh), and
cross-window focus is covered by
[`471_data_drag_hover_focus.sh`](../../tests/harness/checks/471_data_drag_hover_focus.sh).
