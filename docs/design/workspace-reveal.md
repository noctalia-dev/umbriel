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

Each participating root carries its workspace isolation identity and the
transition parameters. At output composition, the capture stack renders each
complete scene, filtering out the other workspace while keeping shared strata in
their native stacking positions. One shader combines the two full-output
textures. Capture neither moves nor enables scene nodes.

`umbriel_sample` reads the outgoing scene and `umbriel_sample_incoming` reads the
incoming scene. UVs cover the logical output. `umbriel_workspace_axis` is the
signed navigation axis. Progress, reversal, cancellation and the transition seed
come from native navigation.

## Blur and capture

Each scene's blur reads its own lower layers. Shared content renders into both
inputs, but emits sampling events and advances feedback only once per output
composition. Both inputs remain live at held progress. The cursor and output
postprocessing remain after the transition. Display and unfiltered capture use
their existing separate histories; feedback is not the incoming scene sampler.
Allocation, texture and capture failures reject the whole reveal frame.

## Coverage

The renderer case checks isolation, feedback and atomic failure. Harness checks
cover live repainting, blur, output transforms/scaling, gestures and lifecycle
cleanup. See [the user guide](../user/animation.md#workspace-reveal) for configuration.
