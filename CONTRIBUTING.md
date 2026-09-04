Contributing
===

This file collects contributor-facing details for Umbriel: design goals, stack notes, code style, source layout,
and debugging helpers. Umbriel shares its conventions with [noctalia](https://github.com/noctalia-dev/noctalia):
same team, same style. If in doubt, match what noctalia does.

For dependencies and normal build commands, start with [README.md](README.md). For what the project accepts and
declines, read [SCOPE.md](SCOPE.md): it is the reference used when triaging feature requests and unsolicited pull
requests.

## Design Principles

- Thin layer over wlroots 0.20 + `umbrielfx`: lean on the libraries, do not reimplement them.
- Domain-oriented C++23: one domain per directory, headers beside their sources, and `src/` as the include root.
- Effects (blur, shadows, rounded corners, animations) go through `umbrielfx`; new visuals land there rather than as
  ad-hoc scene hacks.
- Mechanism and policy stay separate. Example: `View::applySeatFocus` is mechanism; focus policy lives in
  `Server::focusView`.
- Keep the compositor event loop single-threaded. `Server::spawn` relies on that property to make its `fork` and
  environment setup safe.
- Packaging targets Nix first, plus plain system packages via pkg-config.

## Stack

Direct project dependencies. Transitive dependencies are owned by their providing system packages.

| Layer | Library |
|-------|---------|
| Compositor framework | `wlroots-0.20` |
| Scene graph and effects | `umbrielfx` (blur, shadows, rounded corners; maintained in-tree) |
| Wayland core | `wayland-server`, `wayland-client`, `wayland-protocols`, `wayland-scanner` |
| Input | `libinput`, `xkbcommon` |
| Graphics | `pixman`, `libdrm`, OpenGL via wlroots |
| Text | `cairo`, `pangocairo` |
| Memory allocation | `jemalloc` (optional, glibc) |
| Config | `tomlplusplus` |
| JSON (IPC) | `nlohmann/json` |
| Xwayland | `xwayland-satellite` (managed at runtime) |

## Development Commands

The README covers routine builds and running Umbriel. Contributor checks and specialized builds use:

| Command | Purpose |
|---------|---------|
| `just configure <mode> [prefix]` | Create or reconfigure a build directory and symlink `compile_commands.json` to it |
| `just asan` | Build with AddressSanitizer (see [AddressSanitizer](#addresssanitizer)) |
| `just run <mode> [startup]` | Build and run a nested session, optionally spawning a command |
| `just test` | Run the Meson test suite: unit tests plus the umbrielfx suites |
| `just check [filter ...]` | Run the headless compositor harness (`tests/harness/check.sh`), every check or the ones whose names contain a fragment: `just check 721`, `just check drag`, `just check 721 -v`. Checks run several at a time; `-j16` or `CHECK_JOBS=16` changes how many. Another build directory is `mode=`, as in `just mode=asan check 721` |
| `just check-names` | List every harness check name. Builds nothing |
| `just lint` | Rebuild without compiler warnings and run clang-tidy |
| `just format` | Format source and test files |
| `just install` | Build a release binary and install it with `meson install` |
| `just clean <mode>` | Remove a build directory |
| `just rebuild <mode>` | Clean and rebuild a build directory |

Tests live in three places, and which one a change belongs in follows from what it can observe:

```
tests/unit/             C++ unit tests, one binary per test, run by `just test`
tests/meson.build       the unit test table and the harness client targets
tests/harness/check.sh  the headless compositor harness, run by `just check`
tests/harness/checks/   one script per behaviour it asserts
tests/harness/clients/  Wayland helper clients the checks drive
```

A unit test covers math and pure decisions (layout geometry, config classification, keybind parsing) and never needs a
compositor. A harness check covers anything that only exists in a running compositor: real clients, real framebuffers,
seat grabs, live reloads. Every unit test gets `umbriel_pure_dep`, and one that needs compositor code adds
`umbriel_core_dep` in the third field of the `unit_tests` table in `tests/meson.build`. A test that is not in that
table is not built and will rot unnoticed.

Test targets exist only where the `tests` feature option resolves to enabled. `just configure` passes
`-Dtests=enabled` for every mode, so `just test` and `just check` work in debug, asan, and release build directories.
A build directory configured by hand without that option follows `auto`: unsanitized debug builds get the targets, a
release build gets none. Test binaries land in the build directory's `tests` subdir.

`check.sh` runs every script in `tests/harness/checks/` against its own dedicated compositor: one contained headless
instance is booted per check, the check runs in its own process group with `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY`
already pointing at that instance, and the harness kills the group and asserts the instance exited cleanly. Boot plus
teardown costs about 80ms, so isolation is cheaper than the cleanup it replaces. That isolation is also what lets the
harness run several checks at once, bounded by `-j` (default: the core count, capped at eight). Reports stay in
declaration order whatever order the checks finish in. Six rules follow:

- A check must pass in a plain `just check` run, with no environment overrides.
- A check starts from a pristine instance (no windows, overview closed, workspace 1 focused, `$UMBRIEL_CONFIG` holding
  the harness default) and owes nothing to whatever runs next. It appends the config it needs, spawns what it needs,
  and asserts. It must not restore config, close the overview, return to workspace 1, or reap its clients at exit: the
  harness owns all of that. A check that needs a different compositor lifecycle boots a private instance and tears it
  down itself, as `030_session_quit` does.
- Never re-apply `XDG_RUNTIME_DIR`, `WAYLAND_DISPLAY`, or `-u DBUS_SESSION_BUS_ADDRESS` per command. The harness
  already put the body in that environment. This is containment, not convenience: only IPC subcommands honour
  `UMBRIEL_SOCKET`, while `umbriel outputs` and every helper client are Wayland clients resolving `XDG_RUNTIME_DIR`
  and `WAYLAND_DISPLAY`, so a missing prefix used to query the developer's live session instead of the instance.
- Never retain `$!` from a backgrounded shell *function*. Bash forks a subshell, so the captured pid is the wrapper and
  a signal to it leaves the client running. Background the client binary directly when a pid must be kept.
- Never size a wait to an animation. `animation.duration_ms` defaults to 200ms, so a multi-second `sleep` ahead of a
  screenshot is dead time on every run, and it still races a slower machine. Grab until two consecutive frames match
  and keep the fixed wait down to a primer that only covers dispatch, as `650_two_output_containment` does. Its settle
  loop is the barrier; the primers around it are 0.3s.
- Never depend on a machine-wide resource a sibling check could be using at the same time: a fixed port, a shared
  path outside `$UMBRIEL_RUNTIME_DIR`, a named process matched with `pkill`, or the wall-clock cost of a neighbour.
  Everything a check needs lives in its own instance and its own runtime directory.

Check names group by topic, and the leading number is the group: `0xx` session, IPC, and config reload, `1xx` layout,
`2xx` workspaces, `3xx` overview, `4xx` drag, `5xx` input and seat, `6xx` output and display, `7xx` rendering. Numbers
step by ten inside a group so a new check lands next to its relatives.

A boot costs about 80ms and the pool runs checks side by side, so the suite's wall time is set by its longest check,
not by their sum: three six-second siblings finish in six seconds, and folding them into one check makes the whole
suite wait thirteen. Split a check that grows past a few seconds instead of merging relatives to save a boot.

A harness check that asserts a value a unit test computes is coverage in the wrong tier: it costs a compositor to
re-derive what `tests/unit` already pins, and it fails a second time for the same bug. Assert layout arithmetic,
config resolution, and parse results in `tests/unit`, and keep the check for what only the live compositor shows:
that a real client's first configure agrees with the arrangement, that a reload reaches a mapped workspace, that the
pixels land where the geometry said.

An instance has one output unless the check asks for more with a `# harness: outputs=N` directive in its header, which
`620_output_disable`, `630_dpms`, and `650_two_output_containment` use. Output count is fixed when the compositor
starts, so it cannot be a runtime config change. Single-output instances are what `610_output_actions` relies on to
assert that directional output actions are rejected when there is nowhere to move.

A check that stops making progress is killed after 120 seconds, so the suite reports instead of hanging. Set
`CHECK_TIMEOUT` to change the cap, and `CHECK_VERBOSE=1` (or `-v`) to keep the full output of passing checks.

## Code Style

This project uses [clang-format](https://clang.llvm.org/docs/ClangFormat.html) for formatting, with the same
`.clang-format` as noctalia-shell (LLVM base, 2-space indent, 120 columns, left pointer alignment, regrouped includes).
Run `just format` before committing.

Static analysis uses [clang-tidy](https://clang.llvm.org/extra/clang-tidy/) with the same `.clang-tidy` check set as
noctalia-shell. Run `just lint` (warnings are errors). Prefer the modern idioms the checks enforce: `auto`, ranges,
`std::print`/`std::format`, `make_unique`, scoped locks, no C-style casts, uppercase literal suffixes (`1.0F`).

`just configure <mode>` creates a root `compile_commands.json` symlink to the selected Meson build directory, so
clangd and clang-tidy see the build you are working in.

The repo also includes `lefthook.yml`. Run `lefthook install` to install the pre-commit hook; it runs `just format`
and refreshes the git index for tracked formatting changes.

### Naming Conventions

| | Convention | Example |
|---|---|---|
| Files | snake_case | `session_lock.cpp` |
| Directories | snake_case | `input/`, `workspace/` |
| Types / Classes | PascalCase | `SessionLock` |
| Functions / Methods | camelCase | `focusView()` |
| Variables / Parameters | camelCase | `startupCmd` |
| Private members | m_camelCase | `m_sceneTree` |
| Constants | k-prefixed constexpr | `kLayerCount` |
| Macros | SCREAMING_SNAKE_CASE | `UMBRIEL_VERSION` |

Scoped enums (`enum class`) use PascalCase enumerators and an explicit `std::uint8_t` underlying type where it makes
sense: `enum class FocusReason : uint8_t { Directional, PointerPress, ... }`.

Getters are the noun, without a `get` prefix, and `[[nodiscard]]`: `toplevel()`, `mapped()`, `workspace()`.

### wlroots patterns

- Headers use `#pragma once`.
- Forward-declare `wlr_*` structs in headers; include the wlroots headers only in the `.cpp`. Wrap C includes in
  `extern "C" { ... }` when the header is not already C++-safe.
- Wire wlroots signals with the paired-handler pattern: a `static void onEvent(wl_listener*, void*)` trampoline that
  recovers `this` via `wl_container_of` and forwards to a `void handleEvent()` member. Store the `wl_listener` as an
  `m_event{}` member.
- Include ordering follows clang-format regrouping: project `"..."` headers first, then system `<...>` headers.

## Pull Request Template

Pull request descriptions are checked automatically when they are opened, edited, reopened, or marked ready for
review. Keep the `## Summary`, `## Motivation`, `## Type of Change`, `## Testing`, and `## Checklist` headings and the
Checklist wording from [`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md). The remaining sections
are context only: fill them in, leave them empty, or delete them. In Type of Change, keep only the lines that apply.

Draft pull requests may leave checkboxes incomplete. Before marking a pull request ready for review, select at least one
change type and check every item in the Checklist section. A pull request that is missing required template structure
is commented on and converted back to a draft; add the missing content and mark it ready for review to run the check
again. The check never closes a pull request.

## Project Layout

```text
src/
  main.cpp
  wlr.h
  server/     display, backend, scene, protocol wiring, focus, and IPC
  output/     per-output lifecycle and frame commits
  input/      seat, keyboard, cursor, gestures, constraints, and IME relay
  view/       XDG toplevels and popups, window rules, and decoration
  layer/      layer-shell surfaces
  lock/       ext-session-lock surfaces
  xwayland/   xwayland-satellite process supervisor
  workspace/  per-output workspaces and scratchpads
  layout/     scrolling, dwindle, and master layouts, insert and drop targets
  overview/   overview lifecycle and presentation
  scene/      blur, shadows, text, banners, and internal overlays
  config/     TOML parsing, resolution, reloads, and diagnostics
  core/       animation, logging, process, and resource helpers
  cli/        runtime inspection and command-line entry points
umbrielfx/    in-tree scene graph and GLES2 renderer (C, hard fork of SceneFX)
protocols/    vendored Wayland protocol XML
data/         session desktop entry
nix/          package and system integration modules
```

Conventions:

- `src/` is the include root; headers live next to their sources.
- Each directory owns one domain. Add new sources to the matching directory and register them in `meson.build`.
- Vendored Wayland protocol XML lives in `protocols/` and is code-generated via `wayland-scanner` in `meson.build`.
- User-facing configuration documentation lives in [`docs/user/`](docs/user/). Update it when adding or changing
  config options. The reference pages are linked from [`examples/config.toml`](examples/config.toml) and the
  [README](README.md#configuration). Maintainer design notes live in [`docs/design/`](docs/design/).

## umbrielfx

`umbrielfx/` is a hard fork of [SceneFX](https://github.com/wlrfx/scenefx), built by `subdir('umbrielfx')` from the
root `meson.build`. Edit it like any other directory and commit alongside the compositor change that needs it. Never
rebase it onto upstream SceneFX.

It is C compiled against wlroots' private struct layouts (`-DWLR_PRIVATE=`), so its compiler flags stay on its own
target and never reach the compositor's C++23 units. Public headers are `umbrielfx/include/umbrielfx/`; private ones
live in `umbrielfx/internal/` and stay off the compositor's include path. See
[`umbrielfx/README.md`](umbrielfx/README.md).

It replaces wlroots' scene graph but reuses the scene helpers it does not reimplement, such as
`wlr_scene_xdg_surface_create` and `wlr_scene_attach_output_layout`. Those resolve to `libwlroots` and read
umbrielfx's structs at wlroots' field offsets, so a struct in `types/wlr_scene.h` that wlroots also declares must stay
a strict prefix extension: new fields go after every wlroots field. `umbrielfx/tests/abi.c` fails the build's test
suite if that slips.

Its regressions run in their own suite:

```sh
meson test -C build-release --suite umbrielfx
```

## Debugging

- Debug and ASan builds log at debug level to stderr and to `$XDG_CACHE_HOME/umbriel/umbriel.log`
  (fallback `~/.cache/umbriel/umbriel.log`). The first startup record includes the
  release version and commit revision, which helps identify the exact binary
  behind a report.

### AddressSanitizer

`just configure asan` creates `build-asan` as a debug build with `-Db_sanitize=address` and `-Dtests=enabled`, so
every workflow runs there:

```sh
just asan                     # build the instrumented binary
just run asan [startup]       # nested session under ASan
just test asan                # unit tests plus the umbrielfx suites
just mode=asan check [filter] # harness checks, one instrumented compositor per check
```

`b_sanitize` is a global Meson option, so umbrielfx's C sources are instrumented alongside the compositor and a report
points into them with file and line. `just configure` also repoints the `compile_commands.json` symlink, so returning
to unsanitized work needs `just configure debug`.

In asan mode those recipes prepend `abort_on_error=1:detect_leaks=0:halt_on_error=1` to `ASAN_OPTIONS`. A later key
wins, so `ASAN_OPTIONS=detect_leaks=1 just mode=asan check 310` overrides one default and keeps the others.

Leak detection is off because Mesa leaks its EGL display setup on every renderer teardown, under `dri2_initialize`
below `wlr_egl_create_with_drm_fd`: about 400 KB in 7900 allocations when the compositor exits, and 115 KB in the
umbrielfx color tests. Left on, every harness check fails at teardown with `compositor exited with status 1` and the
14 color tests abort. Enable it for the one scope you are chasing a leak in, and read past the `libEGL_mesa` frames.

Reports go to stderr, which for `just run asan` is the parent terminal. Where stderr is not readable, as in a session
started by a display manager, redirect them: `ASAN_OPTIONS=log_path=/var/tmp/umbriel-asan just run asan` writes
`/var/tmp/umbriel-asan.<pid>` instead.

An ASan build still links `jemalloc` when it is installed, since `-Djemalloc` is independent of `b_sanitize`, but
`libasan` precedes it in the link order and services every allocation. The `jemalloc: narenas=...` startup record in
an ASan build therefore describes an allocator nothing uses; configure with `-Djemalloc=disabled` to drop it.

### Runtime inspection

The CLI doubles as a runtime inspection and IPC surface against a running compositor:

```sh
umbriel -v | --version            # print the release version and commit revision
umbriel validate [-c <config>]   # check a config file without starting
umbriel outputs                  # list connectors and modes
umbriel windows                  # list windows (focused *, urgent !)
umbriel workspaces               # list workspaces and their layouts
umbriel subscribe <events>       # stream events as JSON lines until closed
umbriel layers                   # list layer-shell surfaces
umbriel keyboard-layouts         # list configured keyboard layouts
umbriel msg --help              # list actions available to `msg` and keybinds
umbriel msg <action> [args...]   # send an action to the running compositor
```

`windows`, `workspaces`, `layers`, `keyboard-layouts`, and `msg` accept `--json` / `-j` for machine-readable output.
`subscribe` is always JSON; see [docs/user/ipc.md](docs/user/ipc.md) for the families and payloads.

## Commits

Use [Conventional Commits](https://www.conventionalcommits.org/): `type(scope): imperative summary`.
