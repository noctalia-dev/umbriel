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
git_program = find_program('git', required: false)
if git_program.found()
  umbriel_git_revision_h = vcs_tag(
    input: 'src/core/git_revision.h.in',
    output: 'umbriel_git_revision.h',
    command: [git_program, 'describe', '--tags', '--always', '--dirty=-dirty', '--abbrev=12'],
    fallback: 'unknown',
  )
else
  umbriel_git_revision_config = configuration_data()
  umbriel_git_revision_config.set('VCS_TAG', 'unknown')
  umbriel_git_revision_h = configure_file(
    input: 'src/core/git_revision.h.in',
    output: 'umbriel_git_revision.h',
    configuration: umbriel_git_revision_config,
  )
endif

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

wayland_server_dep = dependency('wayland-server', version: '>=1.23')
wayland_client_dep = dependency('wayland-client')
wayland_protocols_dep = dependency('wayland-protocols', version: '>=1.47')
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
    ) and cpp.has_header_symbol(
      'scenefx/types/wlr_scene.h',
      'wlr_scene_tree_set_clip',
      args: scenefx_cflags,
    ) and cpp.has_header_symbol(
      'scenefx/types/wlr_scene.h',
      'wlr_scene_output_set_sdr_white_level',
      args: scenefx_cflags,
    ) and cpp.has_header_symbol(
      'scenefx/types/wlr_scene.h',
      'wlr_scene_buffer_set_luminance_multiplier',
      args: scenefx_cflags,
    ) and cpp.has_member(
      'struct wlr_scene_output_state_options',
      'capture_sdr',
      prefix: '#include <scenefx/types/wlr_scene.h>',
      args: scenefx_cflags,
    ) and cpp.has_header_symbol(
      'scenefx/types/wlr_scene.h',
      'wlr_scene_output_set_direct_scanout_enabled',
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
  scenefx_header_source = fs.read(scenefx_header)
  if (
    not scenefx_header_source.contains('wlr_scene_blur_set_ignore_alpha')
    or not scenefx_header_source.contains('wlr_scene_tree_set_clip')
    or not scenefx_header_source.contains('wlr_scene_output_set_sdr_white_level')
    or not scenefx_header_source.contains('wlr_scene_buffer_set_luminance_multiplier')
    or not scenefx_header_source.contains('capture_sdr')
    or not scenefx_header_source.contains('wlr_scene_output_set_direct_scanout_enabled')
  )
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

foreign_toplevel_client_h = custom_target(
  'wlr-foreign-toplevel-management-unstable-v1-client-protocol.h',
  input: 'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
  output: 'wlr-foreign-toplevel-management-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
foreign_toplevel_client_c = custom_target(
  'wlr-foreign-toplevel-management-unstable-v1-client-protocol.c',
  input: 'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
  output: 'wlr-foreign-toplevel-management-unstable-v1-client-protocol.c',
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

color_management_client_h = custom_target(
  'color-management-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/color-management/color-management-v1.xml',
  output: 'color-management-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
color_management_client_c = custom_target(
  'color-management-v1-client-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/color-management/color-management-v1.xml',
  output: 'color-management-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

tearing_control_client_h = custom_target(
  'tearing-control-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/tearing-control/tearing-control-v1.xml',
  output: 'tearing-control-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
tearing_control_client_c = custom_target(
  'tearing-control-v1-client-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/tearing-control/tearing-control-v1.xml',
  output: 'tearing-control-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

color_management_server_h = custom_target(
  'color-management-v1-server-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/color-management/color-management-v1.xml',
  output: 'color-management-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
color_management_server_c = custom_target(
  'color-management-v1-server-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/color-management/color-management-v1.xml',
  output: 'color-management-v1-server-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

viewporter_client_h = custom_target(
  'viewporter-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'stable/viewporter/viewporter.xml',
  output: 'viewporter-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
viewporter_client_c = custom_target(
  'viewporter-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'stable/viewporter/viewporter.xml',
  output: 'viewporter-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

fractional_scale_client_h = custom_target(
  'fractional-scale-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/fractional-scale/fractional-scale-v1.xml',
  output: 'fractional-scale-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
fractional_scale_client_c = custom_target(
  'fractional-scale-v1-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/fractional-scale/fractional-scale-v1.xml',
  output: 'fractional-scale-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

idle_inhibit_client_h = custom_target(
  'idle-inhibit-unstable-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml',
  output: 'idle-inhibit-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
idle_inhibit_client_c = custom_target(
  'idle-inhibit-unstable-v1-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml',
  output: 'idle-inhibit-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
ext_idle_notify_client_h = custom_target(
  'ext-idle-notify-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/ext-idle-notify/ext-idle-notify-v1.xml',
  output: 'ext-idle-notify-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
ext_idle_notify_client_c = custom_target(
  'ext-idle-notify-v1-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/ext-idle-notify/ext-idle-notify-v1.xml',
  output: 'ext-idle-notify-v1-protocol.c',
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

input_method_client_h = custom_target(
  'input-method-unstable-v2-client-protocol.h',
  input: 'protocols/input-method-unstable-v2.xml',
  output: 'input-method-unstable-v2-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
input_method_client_c = custom_target(
  'input-method-unstable-v2-protocol.c',
  input: 'protocols/input-method-unstable-v2.xml',
  output: 'input-method-unstable-v2-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

ext_workspace_client_h = custom_target(
  'ext-workspace-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/ext-workspace/ext-workspace-v1.xml',
  output: 'ext-workspace-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
ext_workspace_client_c = custom_target(
  'ext-workspace-v1-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/ext-workspace/ext-workspace-v1.xml',
  output: 'ext-workspace-v1-protocol.c',
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
  'src/input/shortcut_keysym.cpp',
  'src/layout/layout.cpp',
  'src/layout/scrolling.cpp',
  'src/layout/dwindle.cpp',
  'src/layout/master.cpp',
  'src/output/direction.cpp',
  'src/scene/cheatsheet_rows.cpp',
  'src/overview/shortcut_labels.cpp',
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
  'src/server/wine_color_manager.cpp',
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
  'src/input/swipe_tracker.cpp',
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
  core_sources + [
    layer_shell_protocol_h,
    output_mgmt_client_h,
    output_mgmt_client_c,
    color_management_server_h,
    color_management_server_c,
  ],
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
  ['src/main.cpp', umbriel_git_revision_h],
  cpp_args: jemalloc_cpp_args,
  dependencies: [umbriel_core_dep] + jemalloc_deps,
  install: true,
)

# Unit tests. Each entry is [name, test sources, extra dependencies]. Every test gets umbriel_pure_dep; add umbriel_core_dep to the third field for a test that
# needs compositor code. Either way a test never lists or recompiles a source.
unit_tests = [
  ['presented-crop', ['tests/unit/presented_crop.cpp'], []],
  ['scene-node', ['tests/unit/scene_node.cpp'], []],
  ['scene-clip', ['tests/unit/scene_clip.cpp'], [wayland_server_dep]],
  ['presentation', ['tests/unit/presentation.cpp'], [umbriel_core_dep]],
  ['animation', ['tests/unit/animation.cpp'], [umbriel_core_dep]],
  ['window-opacity-blur', ['tests/unit/window_opacity_blur.cpp'], [umbriel_core_dep]],
  ['value-parse', ['tests/unit/value_parse.cpp'], []],
  ['keybind-parse', ['tests/unit/keybind_parse.cpp'], []],
  ['modifier-tap', ['tests/unit/modifier_tap.cpp'], []],
  ['shortcut-keysym', ['tests/unit/shortcut_keysym.cpp'], []],
  ['output-direction', ['tests/unit/output_direction.cpp'], []],
  ['output-frame-schedule', ['tests/unit/output_frame_schedule.cpp'], []],
  ['hdr-format', ['tests/unit/hdr_format.cpp'], []],
  ['scrolling-layout', ['tests/unit/scrolling_layout.cpp'], []],
  ['dwindle-layout', ['tests/unit/dwindle_layout.cpp'], []],
  ['master-layout', ['tests/unit/master_layout.cpp'], []],
  ['cheatsheet-rows', ['tests/unit/cheatsheet_rows.cpp'], []],
  ['shortcut-labels', ['tests/unit/shortcut_labels.cpp'], []],
  ['border-ring', ['tests/unit/border_ring.cpp'], []],
  ['floating', ['tests/unit/floating.cpp'], []],
  ['maximize', ['tests/unit/maximize.cpp'], []],
  ['deferred-unfullscreen', ['tests/unit/deferred_unfullscreen.cpp'], []],
  ['config-section', ['tests/unit/config_section.cpp'], []],
  ['config-change', ['tests/unit/config_change.cpp'], []],
  ['config-load', ['tests/unit/config_load.cpp'], [umbriel_core_dep]],
  ['action-registry', ['tests/unit/action_registry.cpp'], [umbriel_core_dep]],
  ['config-resolve', ['tests/unit/config_resolve.cpp'], []],
]

# Harness helper, not a unit test: drives virtual pointer and modifier input against a running compositor. The headless backend has no input devices, so protocol
# input is the only way to exercise pointer hit-testing and grabs.
pointer_client = executable(
  'pointer-client',
  [
    'tests/harness/clients/pointer_client.cpp',
    virtual_pointer_client_h,
    virtual_pointer_client_c,
    virtual_keyboard_client_h,
    virtual_keyboard_client_c,
  ],
  dependencies: [wayland_client_dep, xkbcommon_dep],
)

# Harness helper that reproduces the keyboard ownership of a real input method.
input_method_client = executable(
  'input-method-client',
  [
    'tests/harness/clients/input_method_client.cpp',
    input_method_client_h,
    input_method_client_c,
    virtual_keyboard_client_h,
    virtual_keyboard_client_c,
  ],
  dependencies: [wayland_client_dep],
)

# Harness helper that owns a mapped surface and initiates a data-device drag.
drag_client = executable(
  'drag-client',
  ['tests/harness/clients/drag_client.cpp', layer_shell_client_h, layer_shell_client_c, xdg_shell_client_c],
  dependencies: [wayland_client_dep],
)

# Harness helper that reserves an exclusive zone on a named output. Output
# restoration checks use it to reproduce a panel returning after windows do.
layer_client = executable(
  'layer-client',
  ['tests/harness/clients/layer_client.cpp', layer_shell_client_h, layer_shell_client_c, xdg_shell_client_c],
  dependencies: [wayland_client_dep],
)

# Harness helper that checks whether a Wayland global is visible to a newly
# connected client.
global_client = executable(
  'global-client',
  'tests/harness/clients/global_client.cpp',
  dependencies: [wayland_client_dep],
)

# Harness helper that reports the active workspace through ext-workspace-v1.
workspace_client = executable(
  'workspace-client',
  ['tests/harness/clients/workspace_client.cpp', ext_workspace_client_h, ext_workspace_client_c],
  dependencies: [wayland_client_dep],
)

# Harness helper that checks the output membership advertised for a foreign
# toplevel, including while its workspace is inactive.
foreign_toplevel_client = executable(
  'foreign-toplevel-client',
  [
    'tests/harness/clients/foreign_toplevel_client.cpp',
    foreign_toplevel_client_h,
    foreign_toplevel_client_c,
  ],
  dependencies: [wayland_client_dep],
)

# Harness helper that unmaps its toplevel on a close request without destroying
# it, so the compositor keeps the View registered but unmapped.
unmap_client = executable(
  'unmap-client',
  [
    'tests/harness/clients/unmap_client.cpp',
    xdg_shell_client_h,
    xdg_shell_client_c,
    color_management_client_h,
    color_management_client_c,
    tearing_control_client_h,
    tearing_control_client_c,
  ],
  dependencies: [wayland_client_dep],
)

# Harness helper that creates a grabbed XDG popup and reports its dismissal.
popup_client = executable(
  'popup-client',
  ['tests/harness/clients/popup_client.cpp', xdg_shell_client_h, xdg_shell_client_c],
  dependencies: [wayland_client_dep],
)


# Harness helper whose whole content is drawn by a full-window desynchronized
# wl_subsurface, the way Firefox presents its chrome and web content.
subsurface_client = executable(
  'subsurface-client',
  ['tests/harness/clients/subsurface_client.cpp', xdg_shell_client_h, xdg_shell_client_c],
  dependencies: [wayland_client_dep],
)

# Harness helper that renders one buffer at the preferred fractional scale and
# publishes its logical size through wp_viewporter, the way a fractional-scale
# aware toolkit does.
fractional_client = executable(
  'fractional-client',
  [
    'tests/harness/clients/fractional_client.cpp',
    xdg_shell_client_h,
    xdg_shell_client_c,
    viewporter_client_h,
    viewporter_client_c,
    fractional_scale_client_h,
    fractional_scale_client_c,
  ],
  dependencies: [wayland_client_dep],
)

# Harness helper that keeps an idle inhibitor attached to a mapped toplevel and
# reports when the compositor starts honoring its idle notification.
idle_inhibit_client = executable(
  'idle-inhibit-client',
  [
    'tests/harness/clients/idle_inhibit_client.cpp',
    xdg_shell_client_h,
    xdg_shell_client_c,
    idle_inhibit_client_h,
    idle_inhibit_client_c,
    ext_idle_notify_client_h,
    ext_idle_notify_client_c,
  ],
  dependencies: [wayland_client_dep],
)

alias_target(
  'harness-clients',
  pointer_client,
  input_method_client,
  drag_client,
  layer_client,
  global_client,
  workspace_client,
  foreign_toplevel_client,
  unmap_client,
  popup_client,
  subsurface_client,
  fractional_client,
  idle_inhibit_client,
)

unit_test_targets = []
foreach unit_test : unit_tests
  test_exe = executable(
    unit_test[0] + '-test',
    unit_test[1],
    include_directories: include_directories('tests/unit'),
    dependencies: [umbriel_pure_dep] + unit_test[2],
  )
  unit_test_targets += test_exe
  test(unit_test[0], test_exe)
endforeach

alias_target('unit-tests', unit_test_targets)

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
