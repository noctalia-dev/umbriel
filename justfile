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
        args+=(--buildtype=debug -Db_sanitize=address)
        ;;
      debug)
        args+=(--buildtype=debug)
        ;;
      *)
        # Recipes that build take the mode as their first argument, so a stray
        # argument lands here. Configuring build-{{m}} for it would run whatever
        # follows against a fresh throwaway build directory.
        echo "unknown build mode '{{m}}': expected debug, release, or asan" >&2
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

# Regressions for the GitHub workflow scripts. Pure Python, builds nothing.
test-workflows:
    python3 -m unittest discover -s .github/workflows/scripts -p 'test_*.py'

# Harness checks: the whole suite, or the ones whose names contain any given fragment, each against its own headless compositor instance. `just check 310`, `just check 310 520`, `just check overview`, `just check 310 -v` to keep the output of passing checks. Checks run several at a time; `just check -j16` or `CHECK_JOBS=16` changes how many. Another build directory is `mode=`, as in `just mode=asan check 310`.
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

# Names of every harness check. Boots and builds nothing.
check-names:
    @bash tests/harness/check.sh ./build-{{mode}}/umbriel --list

format:
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 grep -ZlP '\s+$' | xargs -0 -r sed -i 's/[[:space:]]*$//'

# Tests are checked too: they are code the same rules apply to, and a finding
# there is as real as one in src.
_clang_tidy m=mode *args:
    #!/usr/bin/env bash
    set -euo pipefail
    src_root="$(realpath src)"
    tests_root="$(realpath tests)"
    run-clang-tidy -quiet -use-color -p "build-{{m}}" -j "$(nproc)" -header-filter='\.\./(src|tests)/.*' {{args}} "^(${src_root}|${tests_root})/.*"

# Fail on any compiler warning emitted while building. clang-tidy does not surface these: it reports its own check names, not the compiler's diagnostics. Compiles everything rather than only what changed. A warning is emitted when a file is compiled, so an incremental build reports nothing for the files it skipped, which silently turns a gate into a coin flip. A dead function left behind by an edit in another file is exactly the case that slips through.
_warnings m=mode: (_ensure-configured m)
    #!/usr/bin/env bash
    set -euo pipefail
    ninja -C build-{{m}} -t clean >/dev/null
    if ! output=$(ninja -C build-{{m}} 2>&1); then
        printf '%s\n' "$output"
        exit 1
    fi
    if printf '%s\n' "$output" | grep -q 'warning:'; then
        printf '%s\n' "$output" | grep -A8 'warning:'
        echo "error: compiler warnings are not allowed" >&2
        exit 1
    fi

lint m=mode: (_ensure-configured m) (_warnings m)
    just _clang_tidy {{m}} '-warnings-as-errors=*'

clean m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -L compile_commands.json && "$(readlink compile_commands.json)" == "build-{{m}}/compile_commands.json" ]]; then
        rm -f compile_commands.json
    fi
    rm -rf build-{{m}}

rebuild m=mode: (clean m) (build m)
