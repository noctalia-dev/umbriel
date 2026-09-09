# umbrielfx

Umbriel's scene graph and GLES2 renderer, a hard fork of
[wlrfx/scenefx](https://github.com/wlrfx/scenefx).

## Layout

| Path                    | Contents                                          |
| ----------------------- | ------------------------------------------------- |
| `include/umbrielfx/`    | Public API, the only headers the compositor sees   |
| `internal/`             | Private headers, not on the compositor's include path |
| `render/`               | EGL setup, color transforms, pixel formats        |
| `render/fx_renderer/`   | GLES2 renderer, render passes, shaders            |
| `types/`                | Scene graph, output helpers, blur and clip state   |
| `util/`                 | Helpers shared inside the library                  |
| `tests/`                | Color transform, scene ABI, and frame pacing regressions |

## Building

The root `meson.build` pulls this directory in with `subdir('umbrielfx')` and
links the resulting archive into the compositor. Its tests run in the
`umbrielfx` suite:

```sh
meson test -C build --suite umbrielfx
```

Renderer creation owns a DRM file descriptor, which no suite test can check
without a GPU. `tests/renderer.c` builds as the `umbrielfx-renderer-test` tool
for that, run by `just gpu-test`. Run it after a wlroots bump and after any
change to `render/egl.c` or the `fx_renderer_create_*` entry points: it is the
only check that catches a renderer that leaks the descriptor, adopts a
descriptor it should have duplicated, or stops working once a sibling renderer
is destroyed.

## Constraints

- Compiled against wlroots' private struct layouts (`-DWLR_PRIVATE=`), so it is
  pinned to one wlroots minor series.
- C, not C++. Its compiler flags stay on the `umbrielfx_lib` target and must not
  reach the compositor's C++23 translation units.
- Shaders are embedded as generated char arrays, so an installed binary never
  locates shader data files at runtime.
- Replaces wlroots' scene graph completely: every helper that creates or
  destroys scene nodes lives here, and borrowing one from `libwlroots` is a bug.
  `umbrielfx_scene_check_helpers()` refuses to start when one does.
- Structs that wlroots also declares stay a strict prefix extension: fields
  Umbriel adds go after every wlroots field. `tests/abi.c` enforces it.

  Both rules exist for the same reason, and the reason is not obvious. See
  [scene helper ownership](../docs/design/scene-helper-ownership.md).

- A window's desktop and capture scenes share its `wlr_surface`, so
  `types/scene/surface.c` updates only its own scene's output memberships and
  skips suspended outputs when picking the frame-pacing output. Otherwise
  capture replaces the desktop output in `current_outputs` and the client waits
  for frame callbacks that never arrive. Keep it aligned with
  [wlroots 0.20.2 surface.c](https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/0.20.2/types/scene/surface.c);
  `tests/capture_pacing.c` covers it.

## License

MIT, see `LICENSE`. Copyright is held by the SceneFX and wlroots contributors
listed there, plus Umbriel contributors.
