# Scene helper ownership

`umbrielfx` extends wlroots' scene structs. `struct wlr_scene_buffer` there is
480 bytes against wlroots' 456: `fx_corner_radii corners`, `linked_node blur`,
and `float luminance_multiplier` sit after every field wlroots declares.

A node is therefore only usable if `umbrielfx` allocated it. Every scene helper
that creates or destroys nodes has to live in `umbrielfx/types/scene/`, even the
ones that are thin wrappers over wlroots types and carry no effects of their
own: `surface.c`, `subsurface_tree.c`, `xdg_shell.c`, `layer_shell_v1.c`,
`drag_icon.c`, `output_layout.c`. They are vendored from wlroots 0.20.2 and
adapted only in their includes.

## Why borrowing them looked fine

Until they were vendored, those six came from `libwlroots`, and on Arch and Nix
that worked. The compositor links `umbrielfx` statically and so exports
`wlr_scene_buffer_create`; a borrowed helper's internal call to that symbol went
through the PLT, the dynamic linker preferred the executable's definition, and
the node came back at `umbrielfx`'s size.

Nothing in the source said so. The behavior depended entirely on how the
distribution built `libwlroots`.

## Why it broke on Debian and Ubuntu

`dpkg-buildflags` puts `-Wl,-Bsymbolic-functions` in `LDFLAGS` for shared
libraries. It binds a library's references to its own global symbols at link
time, so those internal calls never reach the dynamic linker:

```
$ readelf -r libwlroots-0.20.so | grep JUMP_SLO | grep -c ' wlr_'
0
$ gdb -batch -ex 'disassemble wlr_scene_surface_create' libwlroots-0.20.so | grep buffer_create
   call   0x8b6d0 <wlr_scene_buffer_create>          # direct, not through the PLT
```

Every client surface's node was then allocated by wlroots at 456 bytes while
`umbrielfx` read its own fields at offsets 456-479, past the end of the
allocation. `luminance_multiplier` read back as zero, `tex.frag` multiplies RGB
by it, and **every surface rendered pure black with correct geometry, alpha and
borders**: borders are `umbrielfx`'s own node types, which `umbrielfx`
allocates. `linked_node_destroy(&scene_buffer->blur)` in `umbrielfx`'s
`wlr_scene_node_destroy` never ran either, because a borrowed helper tearing a
node down called wlroots' destroy, so blur lists kept entries into freed memory.

`jemalloc` is why this presented as a clean visual bug rather than a crash: it
rounds the 456-byte request into a 512-byte bin, so the trailing fields landed
in zeroed slack inside the same allocation. Under glibc's allocator
`malloc_usable_size` for that request is exactly 456 and the same code corrupts
the next chunk.

## Do not fix this by

- **Rebuilding wlroots without `-Bsymbolic-functions`.** It is the distribution
  default, not a mistake, and packaged builds are what users install.
- **Making the fields a `wlr_addon` on `scene_buffer->node`.** That survives the
  short allocation, but a borrowed helper still runs wlroots' node destroy and
  wlroots' scene mutation logic on `umbrielfx`'s graph, so the blur bookkeeping
  stays broken.
- **Adding a wlroots subproject fallback to `meson.build`.** It works, but
  `PACKAGING.md` expects a system wlroots, and any packager building against one
  silently reintroduces all of this.

## Guard

`umbrielfx_scene_check_helpers()` runs in `Server::Server()` before
`wlr_scene_create()`. It takes the address of each helper, resolves the module
backing it with `dladdr`, and compares against the module backing `umbrielfx`
itself. A helper served from anywhere else aborts startup by name:

```
umbrielfx scene helpers are not linked correctly: wlr_scene_drag_icon_create
resolves to /usr/lib/x86_64-linux-gnu/libwlroots-0.20.so instead of umbrielfx
(./build-debug/umbriel).
```

To reproduce, drop one of the six from `umbrielfx_sources` in
`umbrielfx/meson.build` and start the compositor.

The guard exists because the failure is otherwise silent: the compositor starts,
accepts clients, answers IPC, and renders nothing but decorations. Keep the
strict prefix property in `tests/abi.c` as well. Nothing in `libwlroots` should
reach these structs now, but the property costs nothing and keeps a reintroduced
helper from corrupting memory before the guard is consulted.

## Scope

This is not specific to Umbriel. Upstream SceneFX ships the same six helpers as
wlroots' and appends `corners` and `blur` to `wlr_scene_buffer`, so any
compositor on it that links a Debian or Ubuntu `libwlroots` has the same
out-of-bounds access, with different symptoms, since it has no
`luminance_multiplier` to zero.
