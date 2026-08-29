// Presents its content the way a fractional-scale-aware toolkit does: it takes the compositor's preferred fractional
// scale, renders one buffer at that exact pixel size, and publishes a logical size through wp_viewporter. The buffer
// size is floored, which is what makes this client useful: a floored buffer is one pixel short of the physical extent
// the compositor derives from the logical window edges, and every renderer shortcut that resolves that disagreement by
// resizing the destination leaves an unpainted row inside the window. Columns alternate blue and green so a screenshot
// shows both coverage (no background pixel inside the content box) and 1:1 sampling (no blended column).
//
// A margin argument surrounds the window with that many logical pixels of fully transparent decoration and declares the
// window geometry inside it, the way a client-side-decorated toolkit reserves room for its shadow. The compositor then
// crops the surface at a boundary that falls between texels, and a renderer that snaps that crop outward samples the
// transparent margin instead of the window: one transparent line along the cropped edge.
//
// Prints "mapped <window_w>x<window_h> buffer <buffer_w>x<buffer_h> surface <surface_w>x<surface_h> margin <m> scale
// <n>/120" for every presentation, so the last line is the layout's size rather than the size the client picked to
// answer an unsized initial configure. Then it keeps the connection alive until the harness kills it.
// Usage: fractional-client [title [default_width default_height [margin]]].

#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {

  constexpr uint32_t kEvenColumn = 0xFF0000FF;
  constexpr uint32_t kOddColumn = 0xFF00FF00;

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_fractional_scale_manager_v1* scaleManager = nullptr;
    wl_surface* surface = nullptr;
    wp_viewport* viewport = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_buffer* buffer = nullptr;
    uint32_t scale = 0; // 1/120ths, as wp_fractional_scale_v1 reports it.
    int defaultWidth = 800;
    int defaultHeight = 600;
    int logicalWidth = 0;
    int logicalHeight = 0;
    bool configured = false;
    // Logical pixels of transparent decoration around the window, the way a client-side-decorated toolkit reserves room
    // for its shadow. The compositor crops the surface to the window, and at a fractional scale that crop edge falls
    // between texels.
    int margin = 0;
  };

  void releaseBuffer(State& state) {
    if (state.buffer != nullptr) {
      wl_buffer_destroy(state.buffer);
      state.buffer = nullptr;
    }
  }

  // Renders one buffer and publishes one logical surface size. The window occupies `margin` logical pixels inside that
  // surface on every side, and everything outside it is fully transparent, the way a client-side-decorated toolkit
  // leaves room for its shadow. A texel belongs to the window only when its center falls inside the window rect, so the
  // boundary texel of a window whose edge lands between texels is transparent, exactly as a real client's is.
  bool present(State& state) {
    if (!state.configured || state.logicalWidth <= 0 || state.logicalHeight <= 0) {
      return true;
    }

    const uint32_t scale = state.scale != 0 ? state.scale : 120;
    const int margin = state.margin;
    const int surfaceWidth = state.logicalWidth + (2 * margin);
    const int surfaceHeight = state.logicalHeight + (2 * margin);
    // Flooring the pixel count is what a toolkit that truncates logical * scale does, and it leaves the buffer's texel
    // grid slightly out of step with the compositor's own scaling of the same logical edges.
    const int bufferWidth = static_cast<int>(static_cast<uint32_t>(surfaceWidth) * scale / 120);
    const int bufferHeight = static_cast<int>(static_cast<uint32_t>(surfaceHeight) * scale / 120);
    if (bufferWidth <= 0 || bufferHeight <= 0) {
      return false;
    }

    const size_t stride = static_cast<size_t>(bufferWidth) * 4;
    const size_t size = stride * static_cast<size_t>(bufferHeight);
    const int fd = memfd_create("umbriel-fractional-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
      if (fd >= 0) {
        close(fd);
      }
      return false;
    }
    void* pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
      close(fd);
      return false;
    }
    auto* words = static_cast<uint32_t*>(pixels);
    // Only texels wholly inside the window carry content. A toolkit that renders its window into a pixel-aligned
    // sub-rect of its buffer leaves the straddling texel to the transparent margin, so the texel the compositor's crop
    // rounds towards is empty rather than a blend.
    const double texelWidth = static_cast<double>(surfaceWidth) / bufferWidth;
    const double texelHeight = static_cast<double>(surfaceHeight) / bufferHeight;
    for (int y = 0; y < bufferHeight; ++y) {
      const double topY = static_cast<double>(y) * texelHeight;
      const bool insideY = topY >= margin && (topY + texelHeight) <= margin + state.logicalHeight;
      for (int x = 0; x < bufferWidth; ++x) {
        const double leftX = static_cast<double>(x) * texelWidth;
        const bool inside = insideY && leftX >= margin && (leftX + texelWidth) <= margin + state.logicalWidth;
        words[(static_cast<size_t>(y) * static_cast<size_t>(bufferWidth)) + static_cast<size_t>(x)] =
            inside ? ((x % 2 == 0) ? kEvenColumn : kOddColumn) : 0x00000000;
      }
    }
    munmap(pixels, size);

    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(size));
    releaseBuffer(state);
    state.buffer =
        wl_shm_pool_create_buffer(pool, 0, bufferWidth, bufferHeight, static_cast<int>(stride), WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (state.buffer == nullptr) {
      return false;
    }

    // The viewport destination is the surface's logical size, so the compositor sizes the window from the geometry
    // below and never from the buffer's pixel count.
    wp_viewport_set_destination(state.viewport, surfaceWidth, surfaceHeight);
    xdg_surface_set_window_geometry(state.xdgSurface, margin, margin, state.logicalWidth, state.logicalHeight);
    wl_surface_attach(state.surface, state.buffer, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, bufferWidth, bufferHeight);
    wl_surface_commit(state.surface);

    // Every presentation reports itself: the first one answers an unsized configure with the client's own default, and
    // the layout's size only arrives once that surface has mapped.
    if (state.scale != 0) {
      std::println(
          "mapped {}x{} buffer {}x{} surface {}x{} margin {} scale {}/120", state.logicalWidth, state.logicalHeight,
          bufferWidth, bufferHeight, surfaceWidth, surfaceHeight, margin, state.scale
      );
      std::fflush(stdout);
    }
    return true;
  }

  void fractionalScalePreferred(void* data, wp_fractional_scale_v1*, uint32_t scale) {
    auto& state = *static_cast<State*>(data);
    if (state.scale == scale) {
      return;
    }
    state.scale = scale;
    if (!present(state)) {
      std::println(stderr, "fractional-client: could not present at scale {}/120", scale);
    }
  }

  constexpr wp_fractional_scale_v1_listener kFractionalScaleListener = {
      .preferred_scale = fractionalScalePreferred,
  };

  void xdgSurfaceConfigure(void* data, xdg_surface* xdgSurface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(xdgSurface, serial);
    state.configured = true;
    if (!present(state)) {
      std::println(stderr, "fractional-client: could not present {}x{}", state.logicalWidth, state.logicalHeight);
    }
  }

  constexpr xdg_surface_listener kXdgSurfaceListener = {
      .configure = xdgSurfaceConfigure,
  };

  // A compositor that leaves the initial configure unsized expects the client to pick, and answers the resulting map
  // with the size its layout assigns.
  void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
    auto& state = *static_cast<State*>(data);
    state.logicalWidth = width > 0 ? width : state.defaultWidth;
    state.logicalHeight = height > 0 ? height : state.defaultHeight;
  }

  void toplevelClose(void*, xdg_toplevel*) {}

  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = nullptr,
      .wm_capabilities = nullptr,
  };

  void wmBasePing(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }

  constexpr xdg_wm_base_listener kWmBaseListener = {
      .ping = wmBasePing,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
    auto& state = *static_cast<State*>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
      state.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
      state.viewporter = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
      state.scaleManager = static_cast<wp_fractional_scale_manager_v1*>(
          wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1)
      );
    }
  }

  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };

} // namespace

int main(int argc, char** argv) {
  const char* title = argc > 1 ? argv[1] : "fractional-client";

  State state;
  if (argc > 3) {
    state.defaultWidth = std::atoi(argv[2]);
    state.defaultHeight = std::atoi(argv[3]);
  }
  if (argc > 4) {
    state.margin = std::atoi(argv[4]);
    if (state.margin < 0) {
      std::println(stderr, "fractional-client: bad margin");
      return 1;
    }
  }
  if (state.defaultWidth <= 0 || state.defaultHeight <= 0) {
    std::println(stderr, "fractional-client: bad default size");
    return 1;
  }
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "fractional-client: no display");
    return 1;
  }

  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  if (state.compositor == nullptr
      || state.shm == nullptr
      || state.wmBase == nullptr
      || state.viewporter == nullptr
      || state.scaleManager == nullptr) {
    std::println(
        stderr, "fractional-client: missing globals: compositor {} shm {} xdg_wm_base {} viewporter {} fractional {}",
        state.compositor != nullptr, state.shm != nullptr, state.wmBase != nullptr, state.viewporter != nullptr,
        state.scaleManager != nullptr
    );
    return 1;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  state.viewport = wp_viewporter_get_viewport(state.viewporter, state.surface);
  wp_fractional_scale_v1* fractionalScale =
      wp_fractional_scale_manager_v1_get_fractional_scale(state.scaleManager, state.surface);
  wp_fractional_scale_v1_add_listener(fractionalScale, &kFractionalScaleListener, &state);

  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgSurfaceListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, title);
  wl_surface_commit(state.surface);

  while (wl_display_dispatch(state.display) >= 0) {
  }
  return 0;
}
