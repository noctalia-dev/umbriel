# Unit tests. Each entry is [name, test sources, extra dependencies]. Every test gets umbriel_pure_dep; add umbriel_core_dep to the third field for a test that
# needs compositor code. Either way a test never lists or recompiles a source.
unit_tests = [
  ['presented-crop', ['unit/presented_crop.cpp'], []],
  ['scene-node', ['unit/scene_node.cpp'], []],
  ['scene-clip', ['unit/scene_clip.cpp'], [wayland_server_dep]],
  ['presentation', ['unit/presentation.cpp'], [umbriel_core_dep]],
  ['animation', ['unit/animation.cpp'], [umbriel_core_dep]],
  ['window-opacity-blur', ['unit/window_opacity_blur.cpp'], [umbriel_core_dep]],
  ['value-parse', ['unit/value_parse.cpp'], []],
  ['keybind-parse', ['unit/keybind_parse.cpp'], []],
  ['modifier-tap', ['unit/modifier_tap.cpp'], []],
  ['keyboard-layout', ['unit/keyboard_layout.cpp'], []],
  ['surface-layouts', ['unit/surface_layouts.cpp'], []],
  ['shortcut-keysym', ['unit/shortcut_keysym.cpp'], []],
  ['output-direction', ['unit/output_direction.cpp'], []],
  ['output-identity', ['unit/output_identity.cpp'], []],
  ['output-frame-schedule', ['unit/output_frame_schedule.cpp'], []],
  ['hdr-format', ['unit/hdr_format.cpp'], []],
  ['layout-struts', ['unit/layout_struts.cpp'], []],
  ['scrolling-layout', ['unit/scrolling_layout.cpp'], []],
  ['dwindle-layout', ['unit/dwindle_layout.cpp'], []],
  ['master-layout', ['unit/master_layout.cpp'], []],
  ['cheatsheet-rows', ['unit/cheatsheet_rows.cpp'], []],
  ['shortcut-labels', ['unit/shortcut_labels.cpp'], []],
  ['border-ring', ['unit/border_ring.cpp'], []],
  ['floating', ['unit/floating.cpp'], []],
  ['preset-cycle', ['unit/preset_cycle.cpp'], []],
  ['maximize', ['unit/maximize.cpp'], []],
  ['deferred-unfullscreen', ['unit/deferred_unfullscreen.cpp'], []],
  ['config-section', ['unit/config_section.cpp'], []],
  ['config-change', ['unit/config_change.cpp'], []],
  ['config-load', ['unit/config_load.cpp'], [umbriel_core_dep]],
  ['config-watcher', ['unit/config_watcher.cpp'], [umbriel_core_dep]],
  ['action-registry', ['unit/action_registry.cpp'], [umbriel_core_dep]],
  ['ipc-commands', ['unit/ipc_commands.cpp'], [umbriel_core_dep]],
  ['config-resolve', ['unit/config_resolve.cpp'], []],
  # Reads docs/user/actions.md from the source tree: the doc's tables are generated from the same spec strings the CLI
  # prints, so the compiled table and the page cannot drift apart unnoticed.
  [
    'docs-actions',
    ['unit/docs_actions.cpp'],
    [],
    ['-DUMBRIEL_DOCS_ACTIONS_MD="' + meson.project_source_root() / 'docs/user/actions.md' + '"'],
  ],
]

unit_test_targets = []
foreach unit_test : unit_tests
  # Optional fourth element: compile args for tests that need a path or flag baked in.
  unit_test_args = unit_test.length() > 3 ? unit_test[3] : []
  test_exe = executable(
    unit_test[0] + '-test',
    unit_test[1],
    include_directories: include_directories('unit'),
    dependencies: [umbriel_pure_dep] + unit_test[2],
    cpp_args: unit_test_args,
  )
  unit_test_targets += test_exe
  test(unit_test[0], test_exe)
endforeach

test(
  'validate-config',
  find_program('validate_config.sh'),
  args: [umbriel],
)

# Harness helper, not a unit test: drives virtual pointer and modifier input against a running compositor. The headless backend has no input devices, so protocol
# input is the only way to exercise pointer hit-testing and grabs.
pointer_client = executable(
  'pointer-client',
  [
    'harness/clients/pointer_client.cpp',
    virtual_pointer_client_h,
    virtual_pointer_client_c,
    virtual_keyboard_client_h,
    virtual_keyboard_client_c,
  ],
  dependencies: [wayland_client_dep, xkbcommon_dep],
)

# Harness helper that checks deferred virtual-keyboard keymaps as seen by regular wl_keyboard clients.
keyboard_keymap_client = executable(
  'keyboard-keymap-client',
  [
    'harness/clients/keyboard_keymap_client.cpp',
    virtual_keyboard_client_h,
    virtual_keyboard_client_c,
  ],
  dependencies: [wayland_client_dep, xkbcommon_dep],
)

# Harness helper that reproduces the keyboard ownership of a real input method.
input_method_client = executable(
  'input-method-client',
  [
    'harness/clients/input_method_client.cpp',
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
  ['harness/clients/drag_client.cpp', layer_shell_client_h, layer_shell_client_c, xdg_shell_client_c],
  dependencies: [wayland_client_dep],
)

# Harness helper that reserves an exclusive zone on a named output. Output
# restoration checks use it to reproduce a panel returning after windows do.
layer_client = executable(
  'layer-client',
  ['harness/clients/layer_client.cpp', layer_shell_client_h, layer_shell_client_c, xdg_shell_client_c],
  dependencies: [wayland_client_dep],
)

# Harness helper that checks whether a Wayland global is visible to a newly
# connected client.
global_client = executable(
  'global-client',
  [
    'harness/clients/global_client.cpp',
    xdg_shell_client_h,
    xdg_shell_client_c,
    xdg_decoration_client_h,
    xdg_decoration_client_c,
  ],
  dependencies: [wayland_client_dep],
)

# Harness helper that sets the regular clipboard through the legacy data-control
# protocol and receives it through the standardized replacement.
data_control_client = executable(
  'data-control-client',
  [
    'harness/clients/data_control_client.cpp',
    wlr_data_control_client_h,
    wlr_data_control_client_c,
    ext_data_control_client_h,
    ext_data_control_client_c,
  ],
  dependencies: [wayland_client_dep],
)

# Harness helper that reports the active workspace through ext-workspace-v1.
workspace_client = executable(
  'workspace-client',
  ['harness/clients/workspace_client.cpp', ext_workspace_client_h, ext_workspace_client_c],
  dependencies: [wayland_client_dep],
)

# Harness helper that checks the output membership advertised for a foreign
# toplevel, including while its workspace is inactive.
foreign_toplevel_client = executable(
  'foreign-toplevel-client',
  [
    'harness/clients/foreign_toplevel_client.cpp',
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
    'harness/clients/unmap_client.cpp',
    xdg_shell_client_h,
    xdg_shell_client_c,
    xdg_activation_client_h,
    xdg_activation_client_c,
    xdg_toplevel_tag_client_h,
    xdg_toplevel_tag_client_c,
    content_type_client_h,
    content_type_client_c,
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
  ['harness/clients/popup_client.cpp', xdg_shell_client_h, xdg_shell_client_c],
  dependencies: [wayland_client_dep],
)


# Harness helper whose whole content is drawn by a full-window desynchronized
# wl_subsurface, the way Firefox presents its chrome and web content.
subsurface_client = executable(
  'subsurface-client',
  ['harness/clients/subsurface_client.cpp', xdg_shell_client_h, xdg_shell_client_c],
  dependencies: [wayland_client_dep],
)

# Harness helper that renders one buffer at the preferred fractional scale and
# publishes its logical size through wp_viewporter, the way a fractional-scale
# aware toolkit does.
fractional_client = executable(
  'fractional-client',
  [
    'harness/clients/fractional_client.cpp',
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
    'harness/clients/idle_inhibit_client.cpp',
    xdg_shell_client_h,
    xdg_shell_client_c,
    idle_inhibit_client_h,
    idle_inhibit_client_c,
    ext_idle_notify_client_h,
    ext_idle_notify_client_c,
  ],
  dependencies: [wayland_client_dep],
)

# Harness helper that locks the session through ext-session-lock and unlocks it
# on demand, so checks can compare compositor state across a real lock cycle.
lock_client = executable(
  'lock-client',
  [
    'harness/clients/lock_client.cpp',
    ext_session_lock_client_h,
    ext_session_lock_client_c,
  ],
  dependencies: [wayland_client_dep],
)

# Harness wrapper that runs another Wayland client through a committed
# security-context listener.
security_context_client = executable(
  'security-context-client',
  [
    'harness/clients/security_context_client.cpp',
    security_context_client_h,
    security_context_client_c,
  ],
  dependencies: [wayland_client_dep],
)

harness_client_targets = [
  pointer_client,
  keyboard_keymap_client,
  input_method_client,
  drag_client,
  layer_client,
  global_client,
  data_control_client,
  workspace_client,
  foreign_toplevel_client,
  unmap_client,
  popup_client,
  subsurface_client,
  fractional_client,
  idle_inhibit_client,
  lock_client,
  security_context_client,
]
