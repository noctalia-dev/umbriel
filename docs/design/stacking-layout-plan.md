# Stacking layout and drop zones

Status: implemented and verified by automated checks. Final clean runs passed all 145 Meson tests and all 411 headless harness checks, with no failures or skips. Native physical-session presentation has not been verified.

This document records the agreed interface, implemented scope, and verification results.

## Result

The workspace layout is named **stacking**, while **floating** describes individual windows. Edge and corner drop zones are configured as a stacking-layout feature instead of an input option.

```toml
[layout]
mode = "stacking"

[layout.stacking]
drop_zones = true
```

Drop zones are enabled by default. `drop_zones = false` is the opt-out. The setting supports both the global layout configuration and existing workspace layout overrides.

The configuration and action cutover removes `input.window_snapping`, replaces the layout token `floating` with `stacking`, and replaces `window-snap` / `window-unsnap` with `window-set-region` / `window-clear-region`. There are no aliases, deprecated readers, or automatic migration.

## Terminology and boundaries

| Concept                                                      | Name after the change | Meaning                                                      |
| :----------------------------------------------------------- | :-------------------- | :----------------------------------------------------------- |
| Workspace with independently positioned, overlapping windows | Stacking layout       | A peer of scrolling, dwindle, and master                     |
| Individual window outside a tiling arrangement               | Floating window       | Still supported above any tiling layout                      |
| Dragging to an edge or corner to select a region             | Drop zones            | Available only in the stacking layout                        |
| Half-screen or quarter-screen window placement               | Edge placement        | Does not make the window a member of a tiling layout         |
| Scene ordering of windows, transients, or layers             | Stacking order        | Existing z-order terminology, independent of the layout name |

Keep window-state vocabulary: `View::floating()`, `WindowDragToggle::Floating`, `default_floating`, `default_floating_size*`, `match.is_floating`, the IPC window `floating` boolean, printed `[float]`, `window-toggle-floating`, and `window-focus-switch-floating`. Keep the `floating` value in `new_exits_fullscreen`, which selects a window kind rather than a layout.

Do not rename floating geometry, positioning, resize, minimize, maximize, or fullscreen helpers merely because they also serve stacking windows. Do not rename master-layout stacks or scene stacking-order helpers.

Use **drop zone** as two words in prose, plural **drop zones**. Use `drop_zone` / `drop_zones` in snake_case identifiers and `DropZone` / `dropZone` / `dropZones` in C++ according to the existing naming convention. Do not introduce the closed forms `dropzone` or `dropzones`. Keep `layout.stacking.drop_zones` as the option name. Use **region** for window placement state and actions; the pointer drop zone selects that region.

## Behavior contract

| Context                                                    | Pointer drop zones                                         | Explicit `window-set-region` action       |
| :--------------------------------------------------------- | :--------------------------------------------------------- | :---------------------------------------- |
| Ordinary eligible window in stacking, `drop_zones = true`  | Preview and apply the selected region                      | Available                                 |
| Ordinary eligible window in stacking, `drop_zones = false` | No preview; preserve the normal free-drag release behavior | Available                                 |
| Floating window above scrolling, dwindle, or master        | Unavailable                                                | Reject without changing geometry or state |
| Tiled, attached, pinned, fullscreen, or scratchpad window  | Preserve existing exclusions                               | Preserve existing exclusions              |

The stacking-only restriction on `window-set-region` is a deliberate behavior change from `window-snap`. Restricting only the cursor would still allow a floating window above a tiling layout to use edge placement. New edge placement belongs exclusively to stacking.

`drop_zones` controls the pointer affordance, not all placement operations. Turning it off must not undo existing placements, disable explicit region actions within stacking, or affect ordinary maximize, fullscreen, movement, or resizing.

### Region actions

```text
window-set-region left
window-set-region top_right
window-clear-region
```

- `window-set-region <region>` selects one of the existing eight halves or quarters. Keep `left`, `right`, `top`, `bottom`, `top_left`, `top_right`, `bottom_left`, and `bottom_right` as the argument values.
- Setting the current region is idempotent: it must not restore the window, overwrite its restore box, or restart an unchanged placement animation. Setting another region changes placement while preserving the original normal position and size.
- `window-clear-region` removes the placement and restores the original normal position and size. Clearing a window with no active region is a no-op.
- Pointer placement uses the same idempotent region-setting operation. Dropping into the same region again must not implicitly restore the window. Restoring free placement is a separate operation.
- Invalid or ineligible set requests must leave geometry, active region, and restore state unchanged. Restore and internal cleanup remain distinct from eligibility to create a placement, as detailed below.

The old repeat-to-restore behavior of `window-snap` is intentionally removed. Do not add a `window-toggle-region` action as part of this change. Reserve “snapping” for a separate potential feature that aligns windows with other windows.

An ordinary eligible window in stacking can use drop zones whether it was explicitly floating beforehand or was floated by the layout. Floating provenance determines what happens when leaving the layout; it must not be used as a proxy for the current workspace mode.

Preserve the current halves on all four edges, quarters in all four corners, client-size limits, usable-area calculations, decorations, gaps, animation origins, and pre-placement restore boxes. All geometry remains in logical coordinates.

## Implemented changes

### 1. Rename the workspace layout throughout the code

- Rename `LayoutMode::Floating` to `LayoutMode::Stacking` in `src/layout/layout.h`.
- Rename `FloatingLayout`, its private `FloatingSnapshot`, and `src/layout/floating.{h,cpp}` to stacking equivalents. Update the factory in `src/layout/layout.cpp` and the Meson source list.
- Update layout-mode branches in workspace, view lifecycle/state, overview, cursor, drop-target, focus, and action code. Search by the enum and by layout-specific prose, rather than replacing every occurrence of `floating`.
- Rename layout-specific provenance and drag members deliberately. The implemented names are `floatedByStackingLayout()` / `m_floatedByStackingLayout`, `floatForStackingLayout()`, `adoptStackingLayoutFloat()`, and `MoveGrab::stackingLayout`. Preserve the provenance lifetime across layout switches and workspace transfers.
- Preserve existing placement, directional focus, cascading, transfers, minimized windows, and fullscreen behavior while renaming. This step must not change individual floating-window policy.
- Rename `tests/unit/floating_layout.cpp` and its Meson target. Rename harness checks whose subject is the workspace mode, such as `layout/floating_mode.sh` and `workspace/floating_transfers.sh`. Keep names for checks about individual floating-window geometry and behavior.

### 2. Update every public layout token together

- Change layout choices in `src/config/fields_layout.cpp` for the global default and workspace rules.
- Update the independent action parser, allowed-value diagnostics, and help in `src/config/keybind_parse.cpp`.
- Update `workspace-set-layout stacking`, custom cycles containing `stacking`, and the default cycle in `src/server/actions.cpp`.
- Update layout labels in `src/scene/cheatsheet_rows.cpp` and the workspace layout serializer in `src/server/ipc_commands.cpp`, including subscription/event payloads that use it.
- Keep per-window floating fields unchanged. A window in a workspace reported as `"layout": "stacking"` may still be reported as `"floating": true`.
- Reject `mode = "floating"`, `workspace-set-layout floating`, and old tokens in custom cycles through normal validation. Preserve valid window-kind uses of `floating`.
- Audited the current local Noctalia Umbriel adapter and broader source for changed layout, region, action, and option names. That checked consumer does not consume the changed names and needs no coordinated source update. No `noctalia-docs` files were edited.

### 3. Replace the region actions and placement vocabulary

- Replace the action registrations, parser enums and argument types, handlers, help, cheatsheet labels, diagnostics, and examples with `window-set-region` / `window-clear-region`. Remove the old action names entirely; both keybind parsing and IPC action dispatch must reject them through normal validation.
- Replace the same-region toggle in `View::snapTo()` with an idempotent setter. Capture the normal restore box only when entering region placement, and retain it through repeated sets, changes between regions, maximize/fullscreen round trips, and pending client configure acknowledgements.
- Do not infer an unchanged placement from the stored region alone. A maximized window can retain region metadata while displaying different geometry; setting that region must leave maximize and apply the requested region through the existing placement policy. Keep the existing exclusion for new placement while fullscreen.
- Rename region-placement code consistently: `SnapRegion` to `WindowRegion`, `snapTo()` to `setRegion()`, and the restoring `unsnap()` operation to `clearRegion()`. Keep a separate `clearRegionState()` operation for forgetting placement metadata without restoring geometry.
- Rename the placement-specific `src/view/snap.{h,cpp}` and `src/view/view_snap.cpp` files and update Meson entries and includes. Update the region geometry helpers, stored region/restore members, and refit helpers to use the same vocabulary.
- Rename pointer preview and dwell helpers to use `DropZone`, for example `updateDropZonePreview()`. Use `region` for the selected placement in the drag state, rather than treating a pointer drop zone as the window's persistent state.
- Rename the per-window IPC placement field from `"snap"` to `"region"`, retaining the existing region values and unplaced representation. Update event payloads, consumers, and harness assertions together; do not publish both fields as an alias.
- Audit by meaning. Leave unrelated animation `snap()` operations, pixel/geometry rounding, and snapshots unchanged; a broad text replacement would corrupt separate concepts.

### 4. Move and resolve the option

- Remove `Config::Input::windowSnapping` and its registration in `src/config/fields_input.cpp`.
- Add `Config::Layout::Stacking::dropZones = true`, an optional workspace override, and the resolved stacking configuration in `src/config/config.h`.
- Register `stacking.drop_zones` through the shared layout fields in `src/config/fields_layout.cpp`. Use the existing `Section` readers and boolean validation, claiming recognized keys before semantic validation can exit.
- Update global resolution, workspace override application, equality/change detection, and reload handling in `src/config/resolve.cpp` and their callers.
- Resolve eligibility from the relevant workspace's effective configuration. Do not read the removed global input flag at the cursor call sites.
- Do not add output-specific stacking overrides as part of this change. The existing output layout overrides have a narrower scope.
- Ensure removed `input.window_snapping` is reported as unknown, even when the new option is also present.

### 5. Apply one eligibility policy to preview and placement

The cursor checks are in `src/input/cursor.cpp`: motion handling, `updateDropZonePreview()`, and `finishFloatMove()`. Region placement and restore checks are in `src/view/view_region.cpp`, with explicit actions in `src/server/actions.cpp`. Workspace/configuration changes refresh stationary previews as well as revalidating release.

- Centralize whether a window may create a new edge placement: its current or intended destination workspace must use stacking, and existing window exclusions must pass.
- Add the effective `stacking.dropZones` condition for pointer previews and pointer release only.
- Re-evaluate the actual destination workspace and its configuration when crossing outputs or workspaces. A drag's saved source-layout flag is not sufficient.
- Clear the preview, shared-edge dwell timer, and cached target when eligibility is lost, including when entering a workspace whose drop zones are disabled.
- Revalidate on release, including when configuration or layout changes while the pointer is stationary. A stale preview must never apply a forbidden placement.
- Cover title-bar moves, client-requested moves, and Mod+drag consistently. Preserve the separate toplevel-drag path's existing behavior.
- Preserve release-position animation continuity and the original restore geometry for eligible stacking drags. Ordinary floating windows above tiling layouts must use the existing free-drag release path.

### 6. Keep placement creation separate from state cleanup

The former `View::canSnap()` predicate also participated in restore and refit operations. Creation eligibility and cleanup are now separate, so a stacking restriction cannot strand existing placement metadata after a workspace or layout change.

- Separate eligibility to create a placement from the ability to restore or clear existing placement state.
- When leaving stacking, clear the active region and dormant region metadata. Windows floated by the layout rejoin the destination tiling layout through the existing provenance policy.
- Explicit floating windows that remain floating retain their current free geometry when leaving stacking. Do not introduce an automatic resize to the old pre-placement box as part of this cleanup.
- Preserve visible maximize/fullscreen state and its valid restore geometry. Cleanup must not call the restoring `window-clear-region` path if doing so would unexpectedly exit those states.
- On stacking-to-stacking transfers, keep existing region refitting and restore behavior, subject to destination constraints.
- Turning only `drop_zones` off leaves existing region state intact. Switching back to stacking after leaving it must not resurrect a stale region.

## Verification

### Configuration and interface checks

Extend existing meaningful checks rather than adding tests for simple renamed getters:

- `tests/unit/config_load.cpp`: new default, explicit false, boolean type errors, workspace inheritance and override, reload behavior, and unknown removed key.
- `tests/unit/keybind_parse.cpp`: `stacking` in direct actions and cycles; parse the new region actions and reject old layout tokens and `window-snap` / `window-unsnap`. Keep independent floating-window vocabulary valid.
- Layout snapshot/factory, cheatsheet, and IPC checks: emit and restore the new mode consistently; publish `region` without the removed `snap` field; window `floating` state retains its meaning.
- Naming review: use “drop zone” / “drop zones”, `drop_zone` / `drop_zones`, and the matching C++ word boundaries throughout new code, docs, and tests.

### Functional regressions

- Adapt and rename the existing `input/snap_*`, `animation/floating_snap_drag`, and `layout/snap_actions` checks to distinguish pointer drop zones from region placement actions. Positive placement fixtures must explicitly select stacking instead of relying on a default tiling workspace with a floating window.
- Verify preview, release, idempotent region setting, explicit clearing/restoration, and animation continuity with drop zones enabled in stacking.
- Exercise normal placement to left, left again, right, right again, then clear. Repeated sets must preserve the selected region and original restore geometry; clear must restore the original position and size, and a repeated clear must leave them unchanged. Also cover pointer release into the same region, repeated set requests during an active animation or pending configure, and setting a retained region while maximized.
- Verify exact free-drag release without a preview when disabled, while explicit region actions remain available in stacking.
- Verify floating windows above each of scrolling, dwindle, and master receive no preview or release placement and reject explicit region actions.
- Verify explicitly floating windows inside stacking remain eligible and retain their floating provenance when leaving it.
- Exercise stacking-to-tiling and tiling-to-stacking drags, cross-output destinations with different settings, stationary release after reload/layout change, and size-constrained windows.
- Exercise leaving stacking with an active placement, including a placed window subsequently maximized or fullscreen. Assert state transitions and restore behavior, not just the final rectangle.
- Retain coverage for shared output edges, scaled outputs, title bars, client move requests, pinned/scratchpad exclusions, and ordinary floating-window resizing.

New and materially changed regression checks were mutation-tested. The implementation was temporarily broken to confirm that checks fail for the intended transitions, then restored before normal validation. Covered regressions include same-region toggling, overwritten restore geometry, missing stacking restrictions, stale drop-zone previews, and layout/workspace cleanup.

No build, runtime, or test dependencies were added.

### Automated results

- Final clean Meson run: 145 of 145 tests passed, with no failures or skips.
- C++ lint: all 27 changed C++ source files passed the repository lint checks.
- C++ formatting: all 35 changed source/header files passed.
- Final clean headless harness run: 411 of 411 checks passed in 174.52 seconds, with no failures or skips.
- Mutation checks: five unit mutations and seven runtime mutations failed for the intended reasons. All mutations were removed; the restored unit checks and seven affected runtime checks passed.
- The example configuration passed validation.
- Documentation formatting and changed shell syntax checks passed.

### Running-session verification

Private headless sessions used the freshly built binary for functional checks. The current layer client sets a positive exclusive zone in `tests/harness/clients/layer_client.cpp`, and `layout/region_refit.sh` verifies region geometry as that panel appears and disappears, including scaled output and title-bar changes.

Native physical-session presentation has not been verified. A running-session check should still assess preview appearance, perceived motion, native Wayland and Xwayland clients, and mixed-scale monitor movement on real displays. Headless buffer and geometry assertions do not establish physical presentation smoothness.

Before changing active configuration, injecting input, or restarting a compositor, ask whether the user prefers to perform the check. Provide a short checklist if they do. Keep private test state isolated, restore temporary changes, and leave a working session running.

## Documentation and release handoff

The implementation updates these user-facing references:

- `docs/user/layout.md`: stacking layout, its relation to individual floating windows, the new option and default, and layout transition behavior.
- `docs/user/input.md`: remove the old setting and point readers to stacking drop zones.
- `docs/user/actions.md`: new layout tokens and cycles, `window-set-region` / `window-clear-region`, idempotent set behavior, stacking-only region placement, and the distinction between explicit actions and the pointer setting.
- `docs/user/ipc.md`: the renamed `region` field and consistent action examples.
- `docs/user/workspaces.md`, `docs/user/index.md`, relevant design notes, and repository overview text: audit layout terminology and examples, including consistent “drop zone” spelling.
- `examples/config.toml`: replace the old input option with a real user-facing `[layout.stacking]` example. Do not use the example to record internal design choices.

Call out the breaking changes explicitly: `floating` becomes `stacking` as a layout name, `input.window_snapping` becomes `layout.stacking.drop_zones`, `window-snap` / `window-unsnap` become `window-set-region` / `window-clear-region`, setting a region no longer toggles restoration, the window IPC field `snap` becomes `region`, and edge-placement creation is restricted to stacking. Retain legitimate floating-window terminology throughout the docs, tests, API, and code.

The implementation uses consistent names and “drop zone” word boundaries across config, actions, help, IPC, implementation, and tests. Automated checks cover stacking-only placement, idempotent set/clear semantics, and preserved floating-window behavior. The remaining verification boundary is native physical-session presentation; final clean full-suite results are recorded above.
