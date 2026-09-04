# Configuration reload

This note records the reload guarantees that maintainers must preserve. The
user-facing configuration guide describes only the outcomes needed while
editing a configuration.

## Core contract

A reload is transactional. Umbriel commits a new configuration only after the
complete file set parses and validates successfully.

If a reload fails:

- The last valid configuration remains active.
- The live compositor state remains unchanged.
- Diagnostics describe the failed attempt.
- Watches follow the attempted include graph. Fixing a broken included file can
  therefore trigger recovery without another edit to the main file.

A successful reload applies only the runtime effects caused by changed
sections. Saving identical content is inert: it must not reset output state,
move focus, close overlays, or otherwise disturb the session.

## Implicit source discovery

Without `-c`, Umbriel captures the lookup candidates from its startup
environment before applying configured environment variables. The order is the
user path, every `$XDG_CONFIG_DIRS` path in order, then the packaged path. Each
automatic or manual reload selects the first regular file from that fixed list.

All candidates remain watched, including paths whose parent directories do not
exist yet. The watcher follows the nearest existing ancestor and rearms as path
components appear. Creating a higher-priority file can therefore promote it
without a restart. Removing the current file selects the next existing
candidate. When no candidate exists, built-in defaults, including the built-in
keybinds, are a successful implicit selection.

Selection is not a parse fallback. If the first existing candidate fails to
parse or validate, the last committed root and configuration remain active;
lower-priority candidates are not loaded. Watches still cover every lookup
candidate and the attempted include graph, so correcting or removing the file
can recover automatically.

An explicit `-c` path stays fixed. A missing or invalid explicit file keeps the
last valid configuration and never enters the implicit lookup chain.

## Selective runtime effects

Output state and workspace inventory are independent effects.

- Changing mode, scale, transform, or position reapplies output state.
- Changing an output's `workspaces` value reconciles its workspace inventory
  and refreshes workspace layout.
- Changing only layout settings refreshes workspace geometry without
  reconciling the inventory or reapplying output state.
- Changing an output's `layout.scrolling.default_width_fraction` refreshes only
  resolved workspace layout configuration. Existing scrolling columns retain
  their stored widths; columns created after the reload use the new default.
- Changing total border width refreshes window decoration and workspace layout
  because borders contribute to resolved tile spacing.
- Changing an output or window-rule tearing policy re-evaluates eligibility,
  clears async recovery state, and schedules a frame. It does not reapply
  output state or invalidate the overview.
- Changing an output's direct scanout policy damages and schedules only outputs
  whose resolved policy changed. It does not reapply output state or invalidate
  the overview.
- `general.autostart` commands run only during startup, never during reload.
- `general.xwayland` changes require a compositor restart.
- `[environment]` values are applied and synchronized to the systemd user
  manager only during startup. A reload does not mutate the compositor, user
  manager, or existing process environments. Traditional D-Bus activation sees
  only the graphical connection variables. Removing a key does not unset a
  value already held by the user manager.

A section can affect more than one runtime consumer. Keep those dependencies
explicit when adding configuration fields, rather than falling back to a full
session refresh.

## Verification

The relevant regression coverage is in:

- [`tests/unit/config_load.cpp`](../../tests/unit/config_load.cpp), which checks
  implicit promotion and fallback, failed-candidate recovery, launch-time path
  capture, and explicit-path pinning.
- [`tests/unit/config_watcher.cpp`](../../tests/unit/config_watcher.cpp), which
  checks automatic promotion when the user config directory does not exist at
  startup.
- [`tests/unit/config_change.cpp`](../../tests/unit/config_change.cpp), which checks
  change classification and runtime effects.
- [`tests/harness/checks/050_config_reload.sh`](../../tests/harness/checks/050_config_reload.sh),
  which checks inert reloads, selective layout updates, border dependencies,
  and recovery after an included file fails to parse.
- [`tests/harness/checks/144_output_scrolling_width.sh`](../../tests/harness/checks/144_output_scrolling_width.sh),
  which checks per-output initial scrolling widths and preserves existing
  column widths when that default changes on reload.
- [`tests/harness/checks/045_session_environment.sh`](../../tests/harness/checks/045_session_environment.sh),
  which checks that environment changes remain unapplied until restart.
