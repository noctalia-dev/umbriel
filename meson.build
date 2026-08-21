project(
  'umbriel',
  ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>=1.3',
  default_options: [
    'cpp_std=c++23',
    'warning_level=2',
    'werror=false',
    'b_ndebug=if-release',
  ],
)

add_project_arguments(
  '-DWLR_USE_UNSTABLE',
  language: 'cpp',
)
add_project_arguments(
  '-DUMBRIEL_VERSION="' + meson.project_version() + '"',
  language: 'cpp',
)
umbriel_data_dir = get_option('prefix') / get_option('datadir')
umbriel_build_config = configuration_data()
umbriel_build_config.set_quoted('UMBRIEL_DATA_DIR', umbriel_data_dir)
configure_file(
  input: 'src/config/build_config.h.in',
  output: 'umbriel_build_config.h',
  configuration: umbriel_build_config,
)
add_project_arguments(
  '-DWLR_USE_UNSTABLE',
  language: 'c',
)

wayland_server_dep = dependency('wayland-server')
wayland_client_dep = dependency('wayland-client')
wayland_protocols_dep = dependency('wayland-protocols', version: '>=1.32')
wlroots_dep = dependency('wlroots-0.20')
# Prefer an installed SceneFX only when it provides Umbriel's patched API.
# Otherwise, build the patched git submodule.
cpp = meson.get_compiler('cpp')
fs = import('fs')
pkgconfig = find_program('pkg-config', required: false)

# jemalloc keeps a long-running compositor's heap compact: fewer arenas and fast page decay bound RSS. It interposes malloc only on glibc, so other libc builds
# skip it (auto) or fail (enabled).
jemalloc_option = get_option('jemalloc')
is_glibc = cpp.get_define('__GLIBC__', prefix: '#include <features.h>') != ''
jemalloc_deps = []
jemalloc_cpp_args = []

if jemalloc_option.enabled() and not is_glibc
  error('jemalloc support requires glibc')
endif

if is_glibc
  jemalloc_dep = dependency('jemalloc', required: jemalloc_option)
  if jemalloc_dep.found()
    jemalloc_deps += [jemalloc_dep]
    jemalloc_cpp_args += ['-DUMBRIEL_USE_JEMALLOC=1']
  endif
endif
use_system_scenefx = false
if pkgconfig.found()
  scenefx_exists = run_command(pkgconfig, '--exists', 'scenefx-0.5', check: false)
  if scenefx_exists.returncode() == 0
    scenefx_cflags = run_command(
      pkgconfig,
      '--cflags',
      'scenefx-0.5',
      check: true,
    ).stdout().strip().split()
    use_system_scenefx = cpp.has_header_symbol(
      'scenefx/types/wlr_scene.h',
      'wlr_scene_blur_set_ignore_alpha',
      args: scenefx_cflags,
    )
  endif
endif

if use_system_scenefx
  scenefx_dep = dependency('scenefx-0.5', allow_fallback: false)
else
  scenefx_header = meson.current_source_dir() / 'subprojects' / 'scenefx' / 'include' / 'scenefx' / 'types' / 'wlr_scene.h'
  if not fs.exists(scenefx_header)
    error('SceneFX submodule is missing, run git submodule update --init')
  endif
  if not fs.read(scenefx_header).contains('wlr_scene_blur_set_ignore_alpha')
    error('SceneFX submodule must provide Umbriel patched APIs')
  endif
  scenefx_proj = subproject(
    'scenefx',
    default_options: ['examples=false', 'tracy_enable=false', 'werror=false', 'default_library=static'],
  )
  scenefx_dep = scenefx_proj.get_variable('scenefx')
endif

xkbcommon_dep = dependency('xkbcommon')
libinput_dep = dependency('libinput', version: '>=1.23')
pixman_dep = dependency('pixman-1')
tomlplusplus_dep = dependency('tomlplusplus')
nlohmann_json_dep = dependency('nlohmann_json')
cairo_dep = dependency('cairo')
pangocairo_dep = dependency('pangocairo')
libdrm_dep = dependency('libdrm')

# wlroots cursor-shape header pulls enum defs from wayland-protocols.
wayland_protocols_enums = declare_dependency(
  include_directories: include_directories(
    wayland_protocols_dep.get_variable(pkgconfig: 'includedir'),
    is_system: true,
  ),
)

wayland_scanner = find_program('wayland-scanner')

layer_shell_protocol_h = custom_target(
  'wlr-layer-shell-unstable-v1-protocol.h',
  input: 'protocols/wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
layer_shell_client_h = custom_target(
  'wlr-layer-shell-unstable-v1-client-protocol.h',
  input: 'protocols/wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
layer_shell_client_c = custom_target(
  'wlr-layer-shell-unstable-v1-client-protocol.c',
  input: 'protocols/wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_h = custom_target(
  'xdg-shell-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml',
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_shell_client_c = custom_target(
  'xdg-shell-client-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml',
  output: 'xdg-shell-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)


output_mgmt_client_h = custom_target(
  'wlr-output-management-unstable-v1-client-protocol.h',
  input: 'protocols/wlr-output-management-unstable-v1.xml',
  output: 'wlr-output-management-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
output_mgmt_client_c = custom_target(
  'wlr-output-management-unstable-v1-protocol.c',
  input: 'protocols/wlr-output-management-unstable-v1.xml',
  output: 'wlr-output-management-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

virtual_pointer_client_h = custom_target(
  'wlr-virtual-pointer-unstable-v1-client-protocol.h',
  input: 'protocols/wlr-virtual-pointer-unstable-v1.xml',
  output: 'wlr-virtual-pointer-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
virtual_pointer_client_c = custom_target(
  'wlr-virtual-pointer-unstable-v1-protocol.c',
  input: 'protocols/wlr-virtual-pointer-unstable-v1.xml',
  output: 'wlr-virtual-pointer-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

virtual_keyboard_client_h = custom_target(
  'virtual-keyboard-unstable-v1-client-protocol.h',
  input: 'protocols/virtual-keyboard-unstable-v1.xml',
  output: 'virtual-keyboard-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
virtual_keyboard_client_c = custom_target(
  'virtual-keyboard-unstable-v1-protocol.c',
  input: 'protocols/virtual-keyboard-unstable-v1.xml',
  output: 'virtual-keyboard-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# Logic with no dependency on the compositor: string parsing and layout geometry. Built once and linked by both the compositor and the tests, so a test never recompiles a translation unit and two tests cannot drift in what they cover. Keep this list free of anything needing SceneFX, pango, or a running server; that is what makes these tests cheap and the coverage honest.
pure_sources = files(
  'src/config/change.cpp',
  'src/config/resolve.cpp',
  'src/config/keybind_parse.cpp',
  'src/config/value_parse.cpp',
  'src/config/section.cpp',
  'src/input/modifier_tap.cpp',
  'src/layout/layout.cpp',
  'src/layout/scrolling.cpp',
  'src/layout/dwindle.cpp',
  'src/scene/cheatsheet_rows.cpp',
  'src/view/border_ring.cpp',
  'src/view/floating.cpp',
)

umbriel_pure = static_library(
  'umbriel-pure',
  pure_sources,
  include_directories: include_directories('src'),
  dependencies: [wlroots_dep, xkbcommon_dep, scenefx_dep, tomlplusplus_dep],
)

# What a test needs to use the pure layer: the library, the include root, and
# the dependencies its headers pull in (scenefx for the border ring's corner radii).
umbriel_pure_dep = declare_dependency(
  link_with: umbriel_pure,
  include_directories: include_directories('src'),
  dependencies: [wlroots_dep, xkbcommon_dep, scenefx_dep, tomlplusplus_dep],
)

# The compositor itself. main.cpp is deliberately not here: everything lives in a library so a test can link any of it without re-listing or recompiling sources,
# and the executable is just an entry point.
core_sources = files(
  'src/core/animation.cpp',
  'src/core/fdlimit.cpp',
  'src/core/log.cpp',
  'src/config/config.cpp',
  'src/config/store.cpp',
  'src/config/config_merge.cpp',
  'src/config/config_watcher.cpp',
  'src/layout/drop_target.cpp',
  'src/server/server.cpp',
  'src/server/server_events.cpp',
  'src/server/server_keybinds.cpp',
  'src/server/server_outputs.cpp',
  'src/server/actions.cpp',
  'src/output/output.cpp',
  'src/input/seat.cpp',
  'src/input/keyboard.cpp',
  'src/input/text_input.cpp',
  'src/input/cursor.cpp',
  'src/input/gestures.cpp',
  'src/input/cursor_constraint.cpp',
  'src/view/view.cpp',
  'src/view/decoration.cpp',
  'src/view/presentation.cpp',
  'src/view/registry.cpp',
  'src/server/focus.cpp',
  'src/xwayland/supervisor.cpp',
  'src/core/process.cpp',
  'src/view/view_foreign.cpp',
  'src/view/popup.cpp',
  'src/layer/layer_surface.cpp',
  'src/scene/surface_blur.cpp',
  'src/scene/hint_rect.cpp',
  'src/scene/border_rect.cpp',
  'src/scene/surface_shadow.cpp',
  'src/scene/config_banner.cpp',
  'src/scene/quit_confirm.cpp',
  'src/scene/cheatsheet.cpp',
  'src/scene/text_buffer.cpp',
  'src/lock/session_lock.cpp',
  'src/overview/overview.cpp',
  'src/workspace/workspace.cpp',
  'src/workspace/scratchpad.cpp',
  'src/cli/outputs.cpp',
  'src/cli/ipc_client.cpp',
  'src/server/ipc.cpp',
  'src/server/ipc_commands.cpp',
)

umbriel_core_deps = [
  scenefx_dep,
  wlroots_dep,
  wayland_server_dep,
  wayland_client_dep,
  wayland_protocols_enums,
  xkbcommon_dep,
  libinput_dep,
  pixman_dep,
  tomlplusplus_dep,
  cairo_dep,
  pangocairo_dep,
  libdrm_dep,
  nlohmann_json_dep,
]

umbriel_core = static_library(
  'umbriel-core',
  core_sources + [layer_shell_protocol_h, output_mgmt_client_h, output_mgmt_client_c],
  include_directories: include_directories('src'),
  link_with: umbriel_pure,
  dependencies: umbriel_core_deps,
)

# A test over compositor code links this and nothing else. It is far heavier than umbriel_pure_dep (SceneFX, pango, cairo), so prefer the pure layer wherever the
# code under test allows it.
umbriel_core_dep = declare_dependency(
  link_with: umbriel_core,
  include_directories: include_directories('src'),
  dependencies: umbriel_core_deps,
)

umbriel = executable(
  'umbriel',
  'src/main.cpp',
  cpp_args: jemalloc_cpp_args,
  dependencies: [umbriel_core_dep] + jemalloc_deps,
  install: true,
)

# Unit tests. Each entry is [name, test sources, extra dependencies]. Every test gets umbriel_pure_dep; add umbriel_core_dep to the third field for a test that
# needs compositor code. Either way a test never lists or recompiles a source.
unit_tests = [
  ['output-clip', ['tests/output_clip.cpp'], []],
  ['scene-node', ['tests/scene_node.cpp'], []],
  ['value-parse', ['tests/value_parse.cpp'], []],
  ['keybind-parse', ['tests/keybind_parse.cpp'], []],
  ['modifier-tap', ['tests/modifier_tap.cpp'], []],
  ['scrolling-layout', ['tests/scrolling_layout.cpp'], []],
  ['dwindle-layout', ['tests/dwindle_layout.cpp'], []],
  ['cheatsheet-rows', ['tests/cheatsheet_rows.cpp'], []],
  ['border-ring', ['tests/border_ring.cpp'], []],
  ['floating', ['tests/floating.cpp'], []],
  ['maximize', ['tests/maximize.cpp'], []],
  ['config-section', ['tests/config_section.cpp'], []],
  ['config-change', ['tests/config_change.cpp'], []],
  ['config-load', ['tests/config_load.cpp'], [umbriel_core_dep]],
  ['action-registry', ['tests/action_registry.cpp'], [umbriel_core_dep]],
  ['config-resolve', ['tests/config_resolve.cpp'], []],
]

# Harness helper, not a unit test: drives virtual pointer and modifier input against a running compositor. The headless backend has no input devices, so protocol
# input is the only way to exercise pointer hit-testing and grabs.
executable(
  'pointer-client',
  [
    'tests/harness/pointer_client.cpp',
    virtual_pointer_client_h,
    virtual_pointer_client_c,
    virtual_keyboard_client_h,
    virtual_keyboard_client_c,
  ],
  dependencies: [wayland_client_dep, xkbcommon_dep],
)

# Harness helper that owns a mapped surface and initiates a data-device drag.
executable(
  'drag-client',
  ['tests/harness/drag_client.cpp', layer_shell_client_h, layer_shell_client_c, xdg_shell_client_c],
  dependencies: [wayland_client_dep],
)

# Harness helper that checks whether a Wayland global is visible to a newly
# connected client.
executable(
  'global-client',
  'tests/harness/global_client.cpp',
  dependencies: [wayland_client_dep],
)

# Harness helper that unmaps its toplevel on a close request without destroying
# it, so the compositor keeps the View registered but unmapped.
executable(
  'unmap-client',
  ['tests/harness/unmap_client.cpp', xdg_shell_client_h, xdg_shell_client_c],
  dependencies: [wayland_client_dep],
)

foreach unit_test : unit_tests
  test_exe = executable(
    unit_test[0] + '-test',
    unit_test[1],
    include_directories: include_directories('tests'),
    dependencies: [umbriel_pure_dep] + unit_test[2],
  )
  test(unit_test[0], test_exe)
endforeach

install_data(
  'data/umbriel.desktop',
  install_dir: get_option('datadir') / 'wayland-sessions',
)

umbriel_session_config = configuration_data()
umbriel_session_config.set(
  'UMBRIEL_EXECUTABLE',
  get_option('prefix') / get_option('bindir') / 'umbriel',
)
configure_file(
  input: 'data/start-umbriel.in',
  output: 'start-umbriel',
  configuration: umbriel_session_config,
  install: true,
  install_dir: get_option('bindir'),
  install_mode: 'rwxr-xr-x',
)
configure_file(
  input: 'data/umbriel.service.in',
  output: 'umbriel.service',
  configuration: umbriel_session_config,
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user',
)

install_data(
  'data/umbriel-session.target',
  'data/umbriel-shutdown.target',
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user',
)

install_data(
  'examples/config.toml',
  install_dir: get_option('datadir') / 'umbriel',
)
