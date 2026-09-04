# umbrielfx is a hard fork of wlrfx/scenefx (MIT, see LICENSE), built as a
# static library rather than joined into the compositor's source lists: it is C
# compiled against wlroots' private struct layouts (-DWLR_PRIVATE=), and those
# flags must not leak into Umbriel's C++23 units.

cc = meson.get_compiler('c')

# dladdr, for the scene helper guard. Folded into libc since glibc 2.34.
umbrielfx_dl_dep = cc.find_library('dl', required: false)
umbrielfx_egl_dep = dependency('egl')
umbrielfx_gbm_dep = dependency('gbm')
umbrielfx_glesv2_dep = dependency('glesv2')
umbrielfx_math_dep = cc.find_library('m')
umbrielfx_rt_dep = cc.find_library('rt')

# Older EGL headers cannot express the dmabuf import and fence attributes
# fx_texture.c and egl.c rely on.
umbrielfx_eglext_version = cc.get_define(
  'EGL_EGLEXT_VERSION',
  dependencies: umbrielfx_egl_dep,
  prefix: '#include <EGL/eglext.h>',
).to_int()
if umbrielfx_eglext_version < 20210604
  error('umbrielfx requires EGL headers with EGL_EGLEXT_VERSION >= 20210604')
endif

umbrielfx_sources = files(
  'render/color.c',
  'render/egl.c',
  'render/pixel_format.c',
  'render/fx_renderer/fx_framebuffer.c',
  'render/fx_renderer/fx_offscreen_buffers.c',
  'render/fx_renderer/fx_pass.c',
  'render/fx_renderer/fx_renderer.c',
  'render/fx_renderer/fx_texture.c',
  'render/fx_renderer/pixel_format.c',
  'render/fx_renderer/shaders.c',
  'render/fx_renderer/tracy.c',
  'render/fx_renderer/util.c',
  'types/fx/blur_data.c',
  'types/fx/clipped_region.c',
  'types/output/output.c',
  'types/scene/drag_icon.c',
  'types/scene/layer_shell_v1.c',
  'types/scene/linked_node.c',
  'types/scene/output_layout.c',
  'types/scene/scene_guard.c',
  'types/scene/subsurface_tree.c',
  'types/scene/surface.c',
  'types/scene/wlr_scene.c',
  'types/scene/xdg_shell.c',
  'util/array.c',
  'util/env.c',
  'util/matrix.c',
  'util/time.c',
)

umbrielfx_deps = [
  libdrm_dep,
  pixman_dep,
  umbrielfx_dl_dep,
  umbrielfx_egl_dep,
  umbrielfx_gbm_dep,
  umbrielfx_glesv2_dep,
  umbrielfx_math_dep,
  umbrielfx_rt_dep,
  wayland_server_dep,
  wlroots_dep,
]

# ICC profile evaluation for client-supplied color management. Without lcms2
# the fallback rejects ICC profiles and leaves the parametric transforms.
umbrielfx_lcms2_dep = dependency('lcms2', required: false)
if umbrielfx_lcms2_dep.found()
  umbrielfx_deps += umbrielfx_lcms2_dep
  umbrielfx_sources += files('render/color_lcms2.c')
else
  umbrielfx_sources += files('render/color_fallback.c')
endif

# wlroots' layer-shell header pulls in the generated server protocol header.
umbrielfx_sources += layer_shell_protocol_h

umbrielfx_little_endian = target_machine.endian() == 'little'

umbrielfx_c_args = [
  '-D_POSIX_C_SOURCE=200809L',
  '-DWLR_USE_UNSTABLE',
  '-DWLR_PRIVATE=',
  '-DWLR_LITTLE_ENDIAN=@0@'.format(umbrielfx_little_endian.to_int()),
  '-DWLR_BIG_ENDIAN=@0@'.format((not umbrielfx_little_endian).to_int()),
]

# Keep wlr_log's __FILE__ output relative to the source root rather than
# leaking the absolute build path.
umbrielfx_relative_dir = fs.relative_to(
  meson.current_source_dir(),
  meson.global_build_root(),
) + '/'
if cc.has_argument('-fmacro-prefix-map=/prefix/to/hide=')
  umbrielfx_c_args += '-fmacro-prefix-map=@0@='.format(umbrielfx_relative_dir)
else
  umbrielfx_c_args += '-D_WLR_REL_SRC_DIR="@0@"'.format(umbrielfx_relative_dir)
endif

umbrielfx_c_args += cc.get_supported_arguments([
  '-Wundef',
  '-Wlogical-op',
  '-Wmissing-include-dirs',
  '-Wold-style-definition',
  '-Wpointer-arith',
  '-Winit-self',
  '-Wstrict-prototypes',
  '-Wimplicit-fallthrough=2',
  '-Wendif-labels',
  '-Wstrict-aliasing=2',
  '-Woverflow',
  '-Wmissing-prototypes',
  '-Walloca',

  '-Wno-missing-braces',
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
])

# Shaders are compiled at runtime, so they ship as generated char arrays rather
# than data files an installed binary would have to locate.
umbrielfx_shader_embed = find_program(
  'render/fx_renderer/shaders/embed.sh',
  native: true,
)
foreach shader : [
  'blur1.frag',
  'blur2.frag',
  'blur_effects.frag',
  'border.frag',
  'box_shadow.frag',
  'common.vert',
  'corner_alpha.frag',
  'gradient.frag',
  'output.frag',
  'quad.frag',
  'quad_grad.frag',
  'quad_grad_round.frag',
  'quad_round.frag',
  'tex.frag',
]
  umbrielfx_sources += custom_target(
    shader.underscorify() + '_src.h',
    command: [umbrielfx_shader_embed, shader.underscorify() + '_src'],
    input: 'render/fx_renderer/shaders' / shader,
    output: shader.underscorify() + '_src.h',
    feed: true,
    capture: true,
  )
endforeach

# Only `include` reaches the compositor. `internal` holds headers whose paths
# (`render/`, `types/`, `util/`) would otherwise sit at the root of every
# compositor translation unit's include path.
umbrielfx_inc = include_directories('include')
umbrielfx_internal_inc = include_directories('internal')

umbrielfx_lib = static_library(
  'umbrielfx',
  umbrielfx_sources,
  c_args: umbrielfx_c_args,
  dependencies: umbrielfx_deps,
  include_directories: [umbrielfx_inc, umbrielfx_internal_inc],
  override_options: [
    'c_std=' + (meson.version().version_compare('>=1.4.0') ? 'c23,c11' : 'c11'),
  ],
  install: false,
)

# link_whole: the scene graph registers node type handlers and format tables
# through constructors and tables the compositor never names directly, so
# archive member pruning would drop them.
umbrielfx_dep = declare_dependency(
  link_whole: umbrielfx_lib,
  dependencies: umbrielfx_deps,
  include_directories: umbrielfx_inc,
)

if build_tests
  umbrielfx_color_test = executable(
    'umbrielfx-color-test',
    'tests/color.c',
    c_args: umbrielfx_c_args,
    dependencies: umbrielfx_deps,
    include_directories: [umbrielfx_inc, umbrielfx_internal_inc],
    link_with: umbrielfx_lib,
    build_by_default: false,
  )
  foreach case_name : [
    'legacy-gamma22-target',
    'srgb-to-gamma22',
    'pq-roundtrip',
    'bt2020-to-srgb',
    'fp16-blending',
    'fp16-blur',
    'fp16-blur-effects',
    'fp16-save-restore',
    'shared-output-buffers',
    'output-lut-cache',
    'shared-sdr-capture',
    'shared-output-transform-transition',
    'shared-sdr-capture-lock',
    'shared-sdr-capture-storage-change',
  ]
    test(
      'color-' + case_name,
      umbrielfx_color_test,
      args: case_name,
      suite: 'umbrielfx',
    )
  endforeach

  # The compositor calls scene helpers umbrielfx does not reimplement, so they
  # resolve to libwlroots and read umbrielfx's structs at wlroots' field offsets.
  # One source, built against each header, proves the offsets still agree.
  umbrielfx_abi_probes = []
  foreach probe : [
    ['wlroots', '<wlr/types/wlr_scene.h>', [wlroots_dep]],
    ['umbrielfx', '<umbrielfx/types/wlr_scene.h>', [wlroots_dep, umbrielfx_dep]],
  ]
    umbrielfx_abi_probes += executable(
      'umbrielfx-abi-probe-' + probe[0],
      'tests/abi.c',
      c_args: umbrielfx_c_args + ['-DABI_PROBE_HEADER=' + probe[1]],
      dependencies: probe[2],
      build_by_default: false,
    )
  endforeach

  test(
    'scene-abi',
    find_program('tests/abi_compare.sh', native: true),
    args: umbrielfx_abi_probes,
    suite: 'umbrielfx',
  )
endif
