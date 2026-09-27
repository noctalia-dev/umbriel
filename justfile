set positional-arguments

mode := "debug"
build-dir := "build-" + mode
prefix := "/usr/local"
cpp-std := "c++23"
# Defaults for asan mode. Leak detection is off: Mesa leaks its EGL setup on
# every renderer teardown, which buries a real finding. A later key wins, so
# ASAN_OPTIONS from the environment is appended and overrides these.
asan-options := "abort_on_error=1:detect_leaks=0:halt_on_error=1"

default:
    @just --list

[no-exit-message]
configure m=mode install_prefix=prefix:
    #!/usr/bin/env bash
    set -euo pipefail
    args=(-Dcpp_std={{cpp-std}} -Dtests=enabled --prefix "{{install_prefix}}")
    case "{{m}}" in
      release)
        args+=(--buildtype=release -Db_lto=true)
        ;;
      asan)
        args+=(--buildtype=debug -Db_sanitize=address -Dwerror=true)
        ;;
      tracy)
        # Packaged Tracy clients are the no-op stub; build one under ~/.local.
        args+=(--buildtype=release -Db_lto=true -Dtracy=enabled)
        args+=(-Dpkg_config_path="$HOME/.local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}")
        ;;
      debug)
        args+=(--buildtype=debug -Dwerror=true)
        ;;
      *)
        # Recipes that build take the mode as their first argument, so a stray
        # argument lands here. Configuring build-{{m}} for it would run whatever
        # follows against a fresh throwaway build directory.
        echo "unknown build mode '{{m}}': expected debug, release, asan, or tracy" >&2
        echo "harness checks select by name, not mode: 'just check {{m}}'" >&2
        exit 2
        ;;
    esac
    if [[ -d "build-{{m}}" ]]; then
        # A build directory rejects an option added since it was configured
        # until Meson re-reads the option file, so regenerate and try again.
        meson setup "build-{{m}}" "${args[@]}" --reconfigure \
          || { meson setup "build-{{m}}" --reconfigure && meson setup "build-{{m}}" "${args[@]}" --reconfigure; }
    else
        meson setup "build-{{m}}" "${args[@]}"
    fi
    ln -sfn "build-{{m}}/compile_commands.json" compile_commands.json

[no-exit-message]
_ensure-configured m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        just configure {{m}}
    fi

build m=mode: (_ensure-configured m)
    meson compile -C build-{{m}} umbriel

debug: (build "debug")

asan: (build "asan")

release: (build "release")

tracy: (build "tracy")

install: (build "release")
    meson install -C build-release --no-rebuild

uninstall:
    sudo ninja -C build-release uninstall

run m=mode startup="": (build m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ "{{m}}" == "asan" ]]; then
        export ASAN_OPTIONS="{{asan-options}}${ASAN_OPTIONS:+:${ASAN_OPTIONS}}"
    fi
    args=()
    if [[ -n "${2:-}" ]]; then
        args=(-s "$2")
    fi
    exec ./build-{{m}}/umbriel "${args[@]}"

test m=mode: (configure m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ "{{m}}" == "asan" ]]; then
        export ASAN_OPTIONS="{{asan-options}}${ASAN_OPTIONS:+:${ASAN_OPTIONS}}"
    fi
    meson compile -C build-{{m}} unit-tests
    meson test -C build-{{m}} --print-errorlogs

# The umbrielfx renderer ownership check needs a real DRM render node, which a
# container or a headless runner does not have, so it is a tool rather than a
# suite entry. This runs it against every render node this machine exposes.
[no-exit-message]
gpu-test m=mode: (_ensure-configured m)
    #!/usr/bin/env bash
    set -euo pipefail
    nodes=(/dev/dri/renderD*)
    if [[ ! -e ${nodes[0]} ]]; then
        echo "no DRM render node under /dev/dri: nothing to check" >&2
        exit 1
    fi
    ninja -C build-{{m}} umbrielfx/umbrielfx-renderer-test
    ./build-{{m}}/umbrielfx/umbrielfx-renderer-test "${nodes[@]}"

# Regressions for the GitHub workflow scripts. Pure Python, builds nothing.
test-workflows:
    python3 -m unittest discover -s .github/workflows/scripts -p 'test_*.py'

# Harness checks: the whole suite, or the ones whose names contain any given fragment, each against its own headless compositor instance. `just check overview/wheel`, `just check overview/wheel input_method`, `just check overview/`, `just check overview/wheel -v` to keep the output of passing checks. Checks run several at a time; `just check -j16` or `CHECK_JOBS=16` changes how many. Another build directory is `mode=`, as in `just mode=asan check overview/wheel`.
[no-exit-message]
check *filters: (_ensure-configured mode)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ "{{mode}}" == "asan" ]]; then
        export ASAN_OPTIONS="{{asan-options}}${ASAN_OPTIONS:+:${ASAN_OPTIONS}}"
    fi
    # Silent unless it fails: the run's own report is the output.
    if ! build_log=$(meson compile -C build-{{mode}} umbriel harness-clients 2>&1); then
        printf '%s\n' "$build_log" >&2
        exit 1
    fi
    bash tests/harness/check.sh ./build-{{mode}}/umbriel {{filters}}

# Runs n copies of one harness check at once, each against its own instance, to expose races that load reveals. `just check-stress focus/dwindle_close`, `just check-stress focus/dwindle_close 64`.
[no-exit-message]
check-stress name n="32": (_ensure-configured mode)
    #!/usr/bin/env bash
    set -euo pipefail
    if ! build_log=$(meson compile -C build-{{mode}} umbriel harness-clients 2>&1); then
        printf '%s\n' "$build_log" >&2
        exit 1
    fi
    mapfile -t matches < <(bash tests/harness/check.sh ./build-{{mode}}/umbriel --list {{name}})
    if ((${#matches[@]} != 1)); then
        echo "check-stress: '{{name}}' must match exactly one check, matched ${#matches[@]}: ${matches[*]}" >&2
        exit 2
    fi
    scratch=$(mktemp -d /tmp/umv-stress.XXXXXXXX)
    trap 'rm -rf "$scratch"' EXIT
    copy=${matches[0]//\//_}
    for ((i = 1; i <= {{n}}; i++)); do
        cp "tests/harness/checks/${matches[0]}.sh" "$scratch/$copy.$(printf '%03d' "$i").sh"
    done
    CHECK_DIR="$scratch" CHECK_DURATIONS_FILE="$scratch/durations" bash tests/harness/check.sh ./build-{{mode}}/umbriel -j {{n}}

# Names of every harness check. Boots and builds nothing.
check-names:
    @bash tests/harness/check.sh ./build-{{mode}}/umbriel --list

format:
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 grep -ZlP '\s+$' | xargs -0 -r sed -i 's/[[:space:]]*$//'

# clang-tidy over src and tests, or only the given files: `just lint`, `just lint src/core/animation.cpp`. Headers are
# checked through the sources that include them. Another build directory is `mode=`, as in `just mode=asan lint`.
# The compile database carries -Werror for the compiler; -Wno-error keeps clang-only warnings out of clang-tidy's errors.
[no-exit-message]
lint *files: (_ensure-configured mode)
    #!/usr/bin/env bash
    set -euo pipefail
    opts=(-quiet -p "build-{{mode}}" -header-filter='\.\./(src|tests)/.*' -warnings-as-errors='*' -extra-arg=-Wno-error)
    if (($# > 0)); then
        exec clang-tidy --use-color "${opts[@]}" "$@"
    fi
    run-clang-tidy -use-color -j "$(nproc)" "${opts[@]}" "^($(realpath src)|$(realpath tests))/.*"

clean m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -L compile_commands.json && "$(readlink compile_commands.json)" == "build-{{m}}/compile_commands.json" ]]; then
        rm -f compile_commands.json
    fi
    rm -rf build-{{m}}

rebuild m=mode: (clean m) (build m)
