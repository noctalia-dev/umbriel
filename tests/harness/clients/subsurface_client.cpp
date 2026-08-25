// Maps an xdg toplevel whose whole visible content is drawn by a desynchronized wl_subsurface covering the parent
// surface exactly, which is how Firefox presents (all chrome and web content live in one MozContainer subsurface over a
// mostly empty GTK parent). The parent buffer is opaque red and the child buffer opaque blue, so a screenshot tells
// which surface a pixel came from and whether the compositor rounded the subsurface to the window corner radius. Prints
// "mapped" once both surfaces are up, then keeps the connection alive until the harness kills it. The optional
// "animate" mode continually damages only the child, matching Firefox's independent MozContainer commits.
// Usage: subsurface-client [title [width height [animate]]]. The dimensions are a fallback: a configure adopts it.

#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {

  struct Buffer {
    wl_buffer* resource = nullptr;
    void* pixels = MAP_FAILED;
    size_t size = 0;
  };

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_surface* child = nullptr;
    wl_subsurface* subsurface = nullptr;
    wl_callback* childFrame = nullptr;
    Buffer parentBuffer;
    Buffer childBuffer;
    int width = 640;
    int height = 480;
    int presentedWidth = 0;
    int presentedHeight = 0;
    bool mapped = false;
    bool animateChild = false;
  };

  void requestChildFrame(State& state);

  void childFrameDone(void* data, wl_callback* callback, uint32_t /*time*/) {
    auto& state = *static_cast<State*>(data);
    wl_callback_destroy(callback);
    state.childFrame = nullptr;
    if (!state.animateChild || state.child == nullptr) {
      return;
    }
    requestChildFrame(state);
    wl_surface_damage_buffer(state.child, 0, 0, state.width, state.height);
    wl_surface_commit(state.child);
  }

  constexpr wl_callback_listener kChildFrameListener = {
      .done = childFrameDone,
  };

  void requestChildFrame(State& state) {
    if (state.childFrame != nullptr) {
      return;
    }
    state.childFrame = wl_surface_frame(state.child);
    wl_callback_add_listener(state.childFrame, &kChildFrameListener, &state);
  }

  void destroyBuffer(Buffer& buffer) {
    if (buffer.resource != nullptr) {
      wl_buffer_destroy(buffer.resource);
      buffer.resource = nullptr;
    }
    if (buffer.pixels != MAP_FAILED) {
      munmap(buffer.pixels, buffer.size);
      buffer.pixels = MAP_FAILED;
    }
    buffer.size = 0;
  }

  Buffer createBuffer(State& state, uint32_t color) {
    Buffer buffer;
    const int stride = state.width * 4;
    buffer.size = static_cast<size_t>(stride) * static_cast<size_t>(state.height);
    const int fd = memfd_create("umbriel-subsurface-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(buffer.size)) < 0) {
      if (fd >= 0) {
        close(fd);
      }
      return buffer;
    }

    buffer.pixels = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer.pixels == MAP_FAILED) {
      close(fd);
      return buffer;
    }
    std::fill_n(static_cast<uint32_t*>(buffer.pixels), buffer.size / sizeof(uint32_t), color);

    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(buffer.size));
    buffer.resource = wl_shm_pool_create_buffer(pool, 0, state.width, state.height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
  }

  // Both surfaces always carry a buffer of the current window size: the compositor's rounding is measured against the
  // window box, so a stale child size would move the corner the check samples. Re-attach only on an actual size change;
  // the previous buffers are released after the commit that replaces them.
  bool presentSize(State& state) {
    if (state.width == state.presentedWidth && state.height == state.presentedHeight) {
      // Nothing to redraw, but the acked configure still needs a commit to be applied.
      wl_surface_commit(state.surface);
      return true;
    }

    Buffer parent = createBuffer(state, 0xFFFF0000);
    Buffer child = createBuffer(state, 0xFF0000FF);
    if (parent.resource == nullptr || child.resource == nullptr) {
      destroyBuffer(parent);
      destroyBuffer(child);
      return false;
    }

    wl_surface_attach(state.child, child.resource, 0, 0);
    wl_surface_damage_buffer(state.child, 0, 0, state.width, state.height);
    if (state.animateChild) {
      requestChildFrame(state);
    }
    wl_surface_commit(state.child);

    wl_surface_attach(state.surface, parent.resource, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, state.width, state.height);
    wl_surface_commit(state.surface);

    destroyBuffer(state.parentBuffer);
    destroyBuffer(state.childBuffer);
    state.parentBuffer = parent;
    state.childBuffer = child;
    state.presentedWidth = state.width;
    state.presentedHeight = state.height;
    return true;
  }

  void xdgSurfaceConfigure(void* data, xdg_surface* xdgSurface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(xdgSurface, serial);
    if (!presentSize(state)) {
      std::println(stderr, "subsurface-client: failed to allocate shared-memory buffers");
      return;
    }
    if (!state.mapped) {
      state.mapped = true;
      std::println("mapped");
      std::fflush(stdout);
    }
  }

  constexpr xdg_surface_listener kXdgSurfaceListener = {
      .configure = xdgSurfaceConfigure,
  };

  void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
    auto& state = *static_cast<State*>(data);
    if (width > 0) {
      state.width = width;
    }
    if (height > 0) {
      state.height = height;
    }
  }

  void toplevelClose(void*, xdg_toplevel*) {}

  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = nullptr,
      .wm_capabilities = nullptr,
  };

  void wmBasePing(void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }

  constexpr xdg_wm_base_listener kWmBaseListener = {
      .ping = wmBasePing,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_subcompositor_interface.name) == 0) {
      state.subcompositor =
          static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
      state.wmBase =
          static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 1U)));
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    }
  }

  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };

} // namespace

int main(int argc, char** argv) {
  State state;
  if (argc > 2) {
    state.width = std::max(1, std::atoi(argv[2]));
  }
  if (argc > 3) {
    state.height = std::max(1, std::atoi(argv[3]));
  }
  state.animateChild = argc > 4 && std::strcmp(argv[4], "animate") == 0;
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "subsurface-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }

  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  wl_display_roundtrip(state.display);

  if (state.compositor == nullptr
      || state.subcompositor == nullptr
      || state.shm == nullptr
      || state.wmBase == nullptr) {
    std::println(stderr, "subsurface-client: compositor is missing a required Wayland global");
    return EXIT_FAILURE;
  }

  const char* title = argc > 1 ? argv[1] : "subsurface-client";
  state.surface = wl_compositor_create_surface(state.compositor);
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgSurfaceListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, title);
  xdg_toplevel_set_app_id(state.toplevel, title);

  // Desynchronized, like Firefox: the child commits its own content without waiting for a parent commit.
  state.child = wl_compositor_create_surface(state.compositor);
  state.subsurface = wl_subcompositor_get_subsurface(state.subcompositor, state.child, state.surface);
  wl_subsurface_set_position(state.subsurface, 0, 0);
  wl_subsurface_set_desync(state.subsurface);

  wl_surface_commit(state.surface);

  while (wl_display_dispatch(state.display) >= 0) {
  }

  if (state.childFrame != nullptr) {
    wl_callback_destroy(state.childFrame);
  }
  if (state.subsurface != nullptr) {
    wl_subsurface_destroy(state.subsurface);
  }
  if (state.child != nullptr) {
    wl_surface_destroy(state.child);
  }
  if (state.toplevel != nullptr) {
    xdg_toplevel_destroy(state.toplevel);
  }
  if (state.xdgSurface != nullptr) {
    xdg_surface_destroy(state.xdgSurface);
  }
  if (state.surface != nullptr) {
    wl_surface_destroy(state.surface);
  }
  destroyBuffer(state.childBuffer);
  destroyBuffer(state.parentBuffer);
  wl_display_disconnect(state.display);
  return EXIT_SUCCESS;
}
