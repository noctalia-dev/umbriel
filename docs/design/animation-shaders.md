# Custom animation shaders

Custom fragment shaders are supported on every animation event. Configuration
and authoring details are in [Animation](../user/animation.md#custom-glsl-shaders).

## Configuration and compilation

`readAnimationShader` uses a `Section` reader for the `shader` file path.
Inline GLSL is not accepted. Paths resolve relative
to the declaring TOML file, including included files. The loader registers file
dependencies even when missing, and includes source contents in configuration
equality. Validation rejects blank/NUL text, nonregular files, and inputs larger
than 256 KiB. Nonblocking file opens prevent FIFOs hanging config reload.

The C++ scene adapter caches one program per event, exact source, and renderer.
Startup and animation config reload prepare programs before rendering. Failures
are cached too, avoiding per-frame compiler retries. UmbrielFX supplies a GLSL
ES 1.00 wrapper around `vec4 animation(vec2 uv)`, normalized target sampling,
logical target size, eased and linear progress, and transition direction.
Compiler diagnostics retain source line numbers and the file/event label.

## Scene processing

Effect state is attached through scene-node addons, preserving the scene ABI.
Nine ordered slots permit simultaneous effects on a node. Descendant effects
run before ancestor effects; same-node order is dimming, border, movement,
opening, closing, scratchpad, layers, workspaces, then overview.

| Event | Target and timeline owner |
| --- | --- |
| `windows_in` | View tree and existing map fade |
| `windows_out` | Close snapshot and existing close fade |
| `windows_move` | View tree and position/presentation-size animation |
| `workspaces` | Output workspace view root and workspace slide |
| `overview` | Per-output overview tree and zoom/row settling |
| `scratchpad` | View show/hide fade and separate dim/blur backdrop targets |
| `border` | Border tree and focus-color animation |
| `dim_unfocused` | View tree and focus-opacity animation |
| `layers` | Layer tree or close snapshot and map/unmap fade |

The renderer captures contiguous descendants from the scene's paint-ordered
render list into an alpha framebuffer, recursively processes inner effects,
then runs each enclosing shader once. Subsurfaces therefore share the window's
effect rather than restarting it. Overview cards reuse the source view's
animation state. Window shadows remain in their native separate stacking tree.

The compositor remains authoritative for geometry, input, clipping, focus,
surface configuration, and lifetime. Lifecycle shaders replace native window
fade/scale/slide visuals; other events postprocess their native presentation.
Logical target bounds are converted to output pixels only at rasterization.
Sampling transforms account for output rotation and fractional scale, with
transparent samples outside target/output bounds. Drawing honors ancestor clips.

Intermediate buffers are pooled per output and nesting depth, allocated on
demand and dropped after effects end. Composition preserves the working format,
including FP16 when color management uses a linear intermediate. Blur within
an effect samples a reconstructed backdrop containing outer captures and earlier
siblings. Texture imports are checked before capture; allocation/import failure
leaves ordinary rendering available. Capture depth is bounded at 24.

While a scene has active effects, opaque-region culling and direct scanout are
disabled and outputs receive full damage. This conservative policy allows
arbitrary target sampling and changes in alpha without stale pixels.

## Lifetime

Nodes and the compilation cache hold independent program references. In-flight
transitions retain their program across source edits until completion or
retargeting. Removing/disabling an effect clears its active slot. Close
snapshots copy current effect parameters, retaining an interrupted opening
effect inside the new closing effect. Layer unmap capture runs before the
scene helper disables its subtree.

Scene destruction releases addon references. Renderer destruction invalidates
remaining programs without accessing a dead context; renderer replacement
prepares new configured programs. Timeline owners clear finished effects and
continue to own cancellation, unmap, and teardown. No shader extends a timeline.

Custom GLSL is trusted local GPU code. Source-size and resource bounds do not
sandbox shader execution or prevent an expensive shader from stalling a driver.

## Regression coverage

`tests/unit/animation_shader.cpp` exercises source validation and file reads.
`tests/unit/config_load.cpp` covers all event sections, included-file provenance,
source-content reload effects, dependency deduplication, and dependency removal.

The isolated running-compositor checks `180_animation_shaders`,
`181_animation_shader_events`, and `182_animation_shader_composition` inspect
shader-specific intermediate pixels, file-watcher reloads, every animation
event, both layer lifecycle directions, rotated fractional-scale UVs, nested
sampling, output containment, and invalid-GLSL fallback. The
`183_animation_shader_lifetime` and `184_animation_squash` checks also cover
program retention across reloads, close-during-open snapshots, shader removal,
and the bundled squash effect's intermediate pixels. Negative controls
temporarily bypass shader rendering and configuration assignment to ensure
the checks fail for the behavior they cover.

Headless checks do not establish physical HDR output correctness or hardware
GPU-reset recovery. Those require suitable hardware and a running-session check.
