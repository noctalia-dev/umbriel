# Maintainer design notes

The main documentation explains how to configure and use Umbriel. The notes in
this directory preserve detailed contracts and implementation constraints for
people changing the compositor.

Start with the user guide when a detail affects configuration or normal use.
Use a design note when the detail explains state transitions, subsystem
boundaries, or regression-sensitive behavior.

- [Configuration reload](configuration-reload.md)
- [Effects](effects.md)
- [Workspace lifecycle](workspace-lifecycle.md)
- [Overview rendering](overview-rendering.md)
- [Border rendering](border-rendering.md)
- [Render performance](render-performance.md)
- [Xwayland input stability](xwayland-input-stability.md)
- [Client buffer constraints](client-buffer-constraints.md)
- [Scene helper ownership](scene-helper-ownership.md)
- [DRM GPU exclusion](drm-device-policy.md)

## Harness-only IPC

`settle`, `clock-freeze`, `clock-advance`, `clock-resume`, `output-create`,
`output-destroy`, `renderer-recover`, and `effect-frames` exist for
`tests/harness` and are compiled only with the `test_ipc` option (auto: debug
builds). `settle` replies once no animation is running, no workspace has an
arrange pending, every mapped window has acknowledged and committed its latest
configure, and every output has drawn a frame since the request; it errors
after 30 seconds.

Every animation ticks from `Server::animationClockMsec`. `clock-freeze` stops it,
`clock-advance <ms>` moves it forward and replies once every output has drawn a
frame at the new time, and `clock-resume` continues from the frozen time so
animation time never runs backwards. An animation that starts while frozen
counts from the frozen instant. Frozen time does not reach clients, input
timestamps, or compositor timers. While an animation runs on the frozen clock,
`settle` replies with an error at the next output frame. `output-create` and
`output-destroy` work only on the headless backend. `renderer-recover` emits
two consecutive notifications through the renderer's real mutable lost signal.
The recovery check uses them to assert that one deferred renderer replacement
completes and draws a new frame. `effect-frames` reports, per output, how many
drawn frames advanced persistent effects' time and how many effect instances
currently need frames of their own.

## Pointer drag completion

A client data-device drag temporarily replaces normal pointer delivery with a
seat grab. When the initiating button release ends that grab,
`Cursor::processButton` reruns pointer motion at the unchanged layout position.
This is required even when the pointer did not move: clients use the fresh
surface-local input to recalculate hover state and restore their cursor image.
When `follows_mouse` is enabled, the same refresh selects a different window
under the pointer and restores keyboard focus there after the drag grab ends.
The short-drag cursor refresh is covered by
[`drag/external_drag.sh`](../../tests/harness/checks/drag/external_drag.sh), and
cross-window focus is covered by
[`drag/data_drag_hover_focus.sh`](../../tests/harness/checks/drag/data_drag_hover_focus.sh).
