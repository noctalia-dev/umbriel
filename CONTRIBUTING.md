Contributing
===

This file collects contributor-facing details for Umbriel: design goals, stack notes, code style, source layout,
and debugging helpers. Umbriel shares its conventions with [noctalia](https://github.com/noctalia-dev/noctalia):
same team, same style. If in doubt, match what noctalia does.

For dependencies and normal build commands, start with [README.md](README.md).

## Design Principles

- Thin layer over wlroots 0.20 + SceneFX: lean on the libraries, do not reimplement them.
- Domain-oriented C++23: one domain per directory, headers beside their sources, and `src/` as the include root.
- Effects (blur, shadows, rounded corners, animations) go through SceneFX; patched APIs live in the fork rather than
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
| Scene graph and effects | `SceneFX` (blur, shadows, rounded corners; patched fork, submodule) |
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
| `just asan` | Build with AddressSanitizer |
| `just run <mode> [startup]` | Build and run a nested session, optionally spawning a command |
| `just test` | Run the Meson test suite |
| `just verify <mode> [filter]` | Run the interactive/visual regression harness (`tests/harness/verify.sh`) against a headless build |
| `just lint` | Rebuild without compiler warnings and run clang-tidy |
| `just format` | Format source and test files |
| `just install` | Build a release binary and install it with `meson install` |
| `just clean <mode>` | Remove a build directory |
| `just rebuild <mode>` | Clean and rebuild a build directory |

Tests live in three places, and which one a change belongs in follows from what it can observe:

```
tests/unit/            C++ unit tests, one binary per test, run by `just test`
tests/harness/verify.sh the headless compositor harness, run by `just verify`
tests/harness/checks/   one script per behaviour it asserts
tests/harness/clients/  Wayland helper clients the checks drive
```

A unit test covers math and pure decisions (layout geometry, config classification, keybind parsing) and never needs a
compositor. A harness check covers anything that only exists in a running compositor: real clients, real framebuffers,
seat grabs, live reloads. Every unit test gets `umbriel_pure_dep`, and one that needs compositor code adds
`umbriel_core_dep` in the third field of the `unit_tests` table in `meson.build`. A test that is not in that table is
not built and will rot unnoticed.

`verify.sh` runs every script in `tests/harness/checks/` against its own dedicated compositor: one contained headless
instance is booted per check, the check runs in its own process group with `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY`
already pointing at that instance, and the harness kills the group and asserts the instance exited cleanly. Boot plus
teardown costs about 80ms, so isolation is cheaper than the cleanup it replaces. Five rules follow:

- A check must pass in a plain `just verify` run, with no environment overrides.
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
- Never size a wait to an animation. `appearance.animation_ms` defaults to 200ms, so a multi-second `sleep` ahead of a
  screenshot is dead time on every run, and it still races a slower machine. Grab until two consecutive frames match
  and keep the fixed wait down to a primer that only covers dispatch, as `650_two_output_containment` does. Its settle
  loop is the barrier; the primers around it are 0.3s.

Check names group by topic, and the leading number is the group: `0xx` session, IPC, and config reload, `1xx` layout,
`2xx` workspaces, `3xx` overview, `4xx` drag, `5xx` input and seat, `6xx` output and display, `7xx` rendering. Numbers
step by ten inside a group so a new check lands next to its relatives.

An instance has one output unless the check asks for more with a `# harness: outputs=N` directive in its header, which
`620_output_disable`, `630_dpms`, and `650_two_output_containment` use. Output count is fixed when the compositor
starts, so it cannot be a runtime config change. Single-output instances are what `610_output_actions` relies on to
assert that directional output actions are rejected when there is nowhere to move.

A check that stops making progress is killed after 120 seconds, so the suite reports instead of hanging. Set
`VERIFY_TIMEOUT` to change the cap, and `VERIFY_VERBOSE=1` (or `-v`) to keep the full output of passing checks.

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
  layout/     scrolling and dwindle layouts, insert and drop targets
  overview/   overview lifecycle and presentation
  scene/      blur, shadows, text, banners, and internal overlays
  config/     TOML parsing, resolution, reloads, and diagnostics
  core/       animation, logging, process, and resource helpers
  cli/        runtime inspection and command-line entry points
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

## SceneFX submodule

SceneFX is a git submodule tracking the `umbriel` branch of `noctalia-dev/scenefx`. Edit its sources in place, commit
in the submodule, and push to the fork.

## Debugging

Debug and ASan builds log at debug level to stderr and to `$XDG_CACHE_HOME/umbriel/umbriel.log`
(fallback `~/.cache/umbriel/umbriel.log`).

Run under AddressSanitizer with `just run asan`.

The CLI doubles as a runtime inspection and IPC surface against a running compositor:

```sh
umbriel validate [-c <config>]   # check a config file without starting
umbriel outputs                  # list connectors and modes
umbriel windows                  # list windows (focused *, urgent !)
umbriel layers                   # list layer-shell surfaces
umbriel keyboard-layouts         # list configured keyboard layouts
umbriel msg --help              # list actions available to `msg` and keybinds
umbriel msg <action> [args...]   # send an action to the running compositor
```

`windows`, `layers`, `keyboard-layouts`, and `msg` accept `--json` / `-j` for machine-readable output.

## Commits

Use [Conventional Commits](https://www.conventionalcommits.org/): `type(scope): imperative summary`.
