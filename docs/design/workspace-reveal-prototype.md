# Live workspace reveal

`WorkspaceGroup` retains native workspace navigation, focus, spring settling and
gesture progress. With `animation.workspaces.style = "reveal"`, participating
normal and fullscreen roots remain at their resting positions and use the existing
`FX_SLOT_WORKSPACES` animation slot. Slide remains the default.

## Existing mechanisms

The effect registry resolves the workspace preset and supplies time uniforms.
`beginAnimationTransition()` supplies the same transition identity and random seed
used by other animation effects. The group retains its shader for the transition;
completion, disable and renderer replacement detach it through the native slide
lifecycle. Failed composition abandons reveal at the current progress and resumes
native slide without committing the incomplete frame.

The shader receives `umbriel_workspace_rect`, the target bounds normalized to the
output, and `umbriel_workspace_axis`, a signed horizontal or vertical axis.
Incoming and outgoing roots share output coordinates and transition identity.
Their `umbriel_direction` values are +1 and -1 respectively. Bounds refresh before
rendering, including while the test animation clock is frozen.

## Blur and capture

An opaque isolation identity groups a workspace's scene roots. The existing
animation capture stack and recursive scene renderer replay the native lower
layers for blur, excluding other workspace groups. Replay does not emit duplicate
client sampling events or advance effect feedback. Shared layers retain normal
stacking. Allocation, texture and capture failures reject the whole reveal frame.

Capture demand uses the existing image-copy session watches and wlroots' public
frame state. A source owns one render lock across its sessions; idle known sources
are discounted once, while unknown locks retain conservative clean composition.
An active session retains reusable clean-capture storage and history between
requests. Only a pending request prepares clean pixels; idle submissions invalidate
old pixels, and final teardown releases capture resources.

## Coverage

The renderer case checks isolation and atomic failure. Harness checks cover live
clients, blur, geometry, gestures and capture-session lifecycle. Pure lock-accounting
cases live in `tests/unit/output_capture_demand.cpp`. See
[validation](workspace-reveal-acceptance.md) for measured results and limits, and
[the user guide](../user/animation.md#workspace-reveal) for configuration.
