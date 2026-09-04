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

# Version floors here are the union of the compositor's needs and umbrielfx's.
wayland_server_dep = dependency('wayland-server', version: '>=1.24')
wayland_client_dep = dependency('wayland-client')
wayland_protocols_dep = dependency('wayland-protocols', version: '>=1.47')
# umbrielfx compiles against wlroots' private struct layouts, so it is pinned to
# one minor series rather than tracking whatever is installed.
wlroots_dep = dependency('wlroots-0.20', version: ['>=0.20.0', '<0.21.0'])
cpp = meson.get_compiler('cpp')
fs = import('fs')

# Unit tests, harness clients, and umbrielfx's checks are defined only when
# `tests` resolves to enabled. Auto mode covers unsanitized debug builds, so a
# distribution release build defines no test targets and `meson install` never
# builds one.
tests_opt = get_option('tests')
build_tests = tests_opt.enabled() or (
  tests_opt.auto()
  and get_option('buildtype') == 'debug'
  and get_option('b_sanitize') == 'none'
)

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

xkbcommon_dep = dependency('xkbcommon')
libinput_dep = dependency('libinput', version: '>=1.23')
pixman_dep = dependency('pixman-1', version: '>=0.43.0')
tomlplusplus_dep = dependency('tomlplusplus')
nlohmann_json_dep = dependency('nlohmann_json')
cairo_dep = dependency('cairo')
pangocairo_dep = dependency('pangocairo')
libdrm_dep = dependency('libdrm', version: '>=2.4.129')

wayland_scanner = find_program('wayland-scanner')

# umbrielfx's vendored layer-shell scene helper needs this server header, so it
# is generated before the subdir that consumes it.
layer_shell_protocol_h = custom_target(
  'wlr-layer-shell-unstable-v1-protocol.h',
  input: 'protocols/wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

# wlroots' layer-shell header includes the generated header by name, so any
# target reaching src/wlr.h needs it generated and on its include path. It lands
# in the build root, which only root-scope targets get for free.
layer_shell_protocol_dep = declare_dependency(
  sources: layer_shell_protocol_h,
  include_directories: include_directories('.'),
)

# Umbriel's fork of the wlroots scene graph and renderer. No installed package
# can supply it, so it is never probed for.
subdir('umbrielfx')

# wlroots cursor-shape header pulls enum defs from wayland-protocols.
wayland_protocols_enums = declare_dependency(
  include_directories: include_directories(
    wayland_protocols_dep.get_variable(pkgconfig: 'includedir'),
    is_system: true,
  ),
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

wlr_data_control_client_h = custom_target(
  'wlr-data-control-unstable-v1-client-protocol.h',
  input: 'protocols/wlr-data-control-unstable-v1.xml',
  output: 'wlr-data-control-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
wlr_data_control_client_c = custom_target(
  'wlr-data-control-unstable-v1-client-protocol.c',
  input: 'protocols/wlr-data-control-unstable-v1.xml',
  output: 'wlr-data-control-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

ext_data_control_client_h = custom_target(
  'ext-data-control-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/ext-data-control/ext-data-control-v1.xml',
  output: 'ext-data-control-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
ext_data_control_client_c = custom_target(
  'ext-data-control-v1-client-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/ext-data-control/ext-data-control-v1.xml',
  output: 'ext-data-control-v1-client-protocol.c',
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

xdg_decoration_client_h = custom_target(
  'xdg-decoration-unstable-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
  output: 'xdg-decoration-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_decoration_client_c = custom_target(
  'xdg-decoration-unstable-v1-client-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
  output: 'xdg-decoration-unstable-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

xdg_activation_client_h = custom_target(
  'xdg-activation-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/xdg-activation/xdg-activation-v1.xml',
  output: 'xdg-activation-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_activation_client_c = custom_target(
  'xdg-activation-v1-client-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/xdg-activation/xdg-activation-v1.xml',
  output: 'xdg-activation-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

xdg_toplevel_tag_client_h = custom_target(
  'xdg-toplevel-tag-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml',
  output: 'xdg-toplevel-tag-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_toplevel_tag_client_c = custom_target(
  'xdg-toplevel-tag-v1-client-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml',
  output: 'xdg-toplevel-tag-v1-client-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

content_type_client_h = custom_target(
  'content-type-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/content-type/content-type-v1.xml',
  output: 'content-type-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
content_type_client_c = custom_target(
  'content-type-v1-client-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/content-type/content-type-v1.xml',
  output: 'content-type-v1-client-protocol.c',
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

ext_session_lock_client_h = custom_target(
  'ext-session-lock-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/ext-session-lock/ext-session-lock-v1.xml',
  output: 'ext-session-lock-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
ext_session_lock_client_c = custom_target(
  'ext-session-lock-v1-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/ext-session-lock/ext-session-lock-v1.xml',
  output: 'ext-session-lock-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

security_context_client_h = custom_target(
  'security-context-v1-client-protocol.h',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/security-context/security-context-v1.xml',
  output: 'security-context-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
security_context_client_c = custom_target(
  'security-context-v1-protocol.c',
  input: wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir') / 'staging/security-context/security-context-v1.xml',
  output: 'security-context-v1-protocol.c',
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

# Logic with no dependency on the compositor: string parsing and layout geometry. Built once and linked by both the compositor and the tests, so a test never recompiles a translation unit and two tests cannot drift in what they cover. Keep this list free of anything needing umbrielfx, pango, or a running server; that is what makes these tests cheap and the coverage honest.
pure_sources = files(
  'src/config/change.cpp',
  'src/config/resolve.cpp',
  'src/config/keybind_parse.cpp',
  'src/config/value_parse.cpp',
  'src/config/section.cpp',
  'src/input/modifier_tap.cpp',
  'src/input/keyboard_layout.cpp',
  'src/input/surface_layouts.cpp',
  'src/input/shortcut_keysym.cpp',
  'src/layout/layout.cpp',
  'src/layout/scrolling.cpp',
  'src/layout/dwindle.cpp',
  'src/layout/master.cpp',
  'src/output/direction.cpp',
  'src/output/identity.cpp',
  'src/scene/cheatsheet_rows.cpp',
  'src/overview/shortcut_labels.cpp',
  'src/view/border_ring.cpp',
  'src/view/floating.cpp',
)

umbriel_pure_deps = [
  wlroots_dep,
  xkbcommon_dep,
  umbrielfx_dep,
  tomlplusplus_dep,
  layer_shell_protocol_dep,
]

umbriel_pure = static_library(
  'umbriel-pure',
  pure_sources,
  include_directories: include_directories('src'),
  dependencies: umbriel_pure_deps,
)

# What a test needs to use the pure layer: the library, the include root, and
# the dependencies its headers pull in (umbrielfx for the border ring's corner radii).
umbriel_pure_dep = declare_dependency(
  link_with: umbriel_pure,
  include_directories: include_directories('src'),
  dependencies: umbriel_pure_deps,
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
  umbrielfx_dep,
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
  layer_shell_protocol_dep,
]

umbriel_core = static_library(
  'umbriel-core',
  core_sources + [
    output_mgmt_client_h,
    output_mgmt_client_c,
    color_management_server_h,
    color_management_server_c,
  ],
  include_directories: include_directories('src'),
  link_with: umbriel_pure,
  dependencies: umbriel_core_deps,
)

# A test over compositor code links this and nothing else. It is far heavier than umbriel_pure_dep (umbrielfx, pango, cairo), so prefer the pure layer wherever the
# code under test allows it.
umbriel_core_dep = declare_dependency(
  link_with: umbriel_core,
  include_directories: include_directories('src'),
  dependencies: umbriel_core_deps,
)

# build_info.cpp is the only unit compiled against the generated revision header
# and the version define, and it stays out of umbriel_core so a new commit
# rebuilds one small object and relinks the compositor instead of the library
# every test links.
umbriel = executable(
  'umbriel',
  ['src/main.cpp', 'src/core/build_info.cpp', umbriel_git_revision_h],
  cpp_args: jemalloc_cpp_args + ['-DUMBRIEL_VERSION="' + meson.project_version() + '"'],
  dependencies: [umbriel_core_dep] + jemalloc_deps,
  install: true,
)

# Unit tests and the harness clients check.sh drives.
if build_tests
  subdir('tests')
  alias_target('unit-tests', unit_test_targets)
  alias_target('harness-clients', harness_client_targets)
endif

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
