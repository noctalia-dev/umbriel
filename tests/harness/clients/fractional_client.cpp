// Takes the compositor's preferred fractional scale, renders one buffer at that pixel size (floored), and publishes a
// logical size through wp_viewporter. The floored buffer is one pixel short of the physical window extent, so a
// renderer that resizes the destination leaves an unpainted row. Columns alternate blue and green so a screenshot shows
// both coverage and 1:1 sampling (no blended column).
//
// A margin argument surrounds the window with that many logical pixels of transparent decoration and declares the
// window geometry inside it; at a fractional scale the compositor's crop then falls between texels, and a crop snapped
// outward shows one transparent line along the cropped edge.
//
// Prints "mapped <window_w>x<window_h> buffer <buffer_w>x<buffer_h> surface <surface_w>x<surface_h> margin <m> scale
// <n>/120" for every presentation, so the last line is the layout's size rather than the size the client picked to
// answer an unsized initial configure. Then it keeps the connection alive until the harness kills it.
// Usage: fractional-client [title [default_width default_height [margin]]].
// UNMAPPED_SUBSURFACE adds an unbuffered child at (-200, -200), as Firefox does.
// HOLD_SIZE keeps the default dimensions while acknowledging configures, exposing compositor-only resize scaling.
// TITLE_AFTER_CONFIGURE sets the opening title after the first configure and before committing the first buffer.
// RESIZE_BEFORE_MAP uses the default dimensions for that first buffer, then follows later configures normally.

#include "fractional-scale-v1-client-protocol.h"
#include "parse_ints.h"
#include "server-decoration-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {

  constexpr uint32_t kEvenColumn = 0xFF0000FF;
  constexpr uint32_t kOddColumn = 0xFF00FF00;

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_fractional_scale_manager_v1* scaleManager = nullptr;
    wl_surface* surface = nullptr;
    wp_viewport* viewport = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_buffer* buffer = nullptr;
    zxdg_decoration_manager_v1* decorationManager = nullptr;
    zxdg_toplevel_decoration_v1* decoration = nullptr;
    org_kde_kwin_server_decoration_manager* legacyManager = nullptr;
    org_kde_kwin_server_decoration* legacyDecoration = nullptr;
    wl_pointer* pointer = nullptr;
    std::string commands;
    bool running = true;
    uint32_t scale = 0; // 1/120ths, as wp_fractional_scale_v1 reports it.
    int defaultWidth = 800;
    int defaultHeight = 600;
    int logicalWidth = 0;
    int logicalHeight = 0;
    bool configured = false;
    // Logical pixels of transparent decoration around the window geometry.
    int margin = 0;
  };

  void releaseBuffer(State& state) {
    if (state.buffer != nullptr) {
      wl_buffer_destroy(state.buffer);
      state.buffer = nullptr;
    }
  }

  // Renders one buffer and publishes one logical surface size. The window occupies `margin` logical pixels inside that
  // surface on every side; everything outside it is transparent. A texel belongs to the window only when its center
  // falls inside the window rect, so a boundary texel straddling the window edge is transparent.
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
    if (!state.configured) {
      if (const char* title = std::getenv("TITLE_AFTER_CONFIGURE")) {
        xdg_toplevel_set_title(state.toplevel, title);
        std::println("title-after-configure {}", title);
        std::fflush(stdout);
      }
      if (std::getenv("RESIZE_BEFORE_MAP") != nullptr) {
        state.logicalWidth = state.defaultWidth;
        state.logicalHeight = state.defaultHeight;
      }
    }
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
    if (std::getenv("LOG_CONFIGURES") != nullptr) {
      std::println("configure {}x{}", width, height);
      std::fflush(stdout);
    }
    const bool holdSize = std::getenv("HOLD_SIZE") != nullptr;
    state.logicalWidth = !holdSize && width > 0 ? width : state.defaultWidth;
    state.logicalHeight = !holdSize && height > 0 ? height : state.defaultHeight;
  }

  void toplevelClose(void* data, xdg_toplevel*) { static_cast<State*>(data)->running = false; }

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

  const wl_pointer_listener kPointerListener = [] {
    wl_pointer_listener listener{};
    listener.enter = [](void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {};
    listener.leave = [](void*, wl_pointer*, uint32_t, wl_surface*) {};
    listener.motion = [](void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {};
    listener.button = [](void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
      std::println("pointer-button {} {}", button, state);
      std::fflush(stdout);
    };
    listener.axis = [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {};
    return listener;
  }();
  void seatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) {
    auto& state = *static_cast<State*>(data);
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && state.pointer == nullptr) {
      state.pointer = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(state.pointer, &kPointerListener, &state);
    } else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0 && state.pointer != nullptr) {
      wl_pointer_destroy(state.pointer);
      state.pointer = nullptr;
    }
  }
  constexpr wl_seat_listener kSeatListener{.capabilities = seatCapabilities, .name = nullptr};
  void decorationConfigure(void*, zxdg_toplevel_decoration_v1*, uint32_t mode) {
    std::println("decoration {}", mode);
    std::fflush(stdout);
  }
  constexpr zxdg_toplevel_decoration_v1_listener kDecorationListener{.configure = decorationConfigure};

  void legacyMode(void*, org_kde_kwin_server_decoration*, uint32_t mode) {
    std::println("legacy-decoration {}", mode);
    std::fflush(stdout);
  }
  constexpr org_kde_kwin_server_decoration_listener kLegacyListener{.mode = legacyMode};

  void handleDecorationCommand(State& state, std::string_view command) {
    if (command == "xdg destroy" && state.decoration != nullptr) {
      zxdg_toplevel_decoration_v1_destroy(state.decoration);
      state.decoration = nullptr;
    } else if (command == "legacy destroy" && state.legacyDecoration != nullptr) {
      org_kde_kwin_server_decoration_release(state.legacyDecoration);
      state.legacyDecoration = nullptr;
    } else if (command.starts_with("xdg ") && state.decoration != nullptr) {
      if (command == "xdg unset")
        zxdg_toplevel_decoration_v1_unset_mode(state.decoration);
      else
        zxdg_toplevel_decoration_v1_set_mode(state.decoration, command == "xdg server" ? 2 : 1);
    } else if (command.starts_with("legacy ") && state.legacyDecoration != nullptr) {
      const uint32_t mode = command == "legacy server" ? 2 : (command == "legacy none" ? 0 : 1);
      org_kde_kwin_server_decoration_request_mode(state.legacyDecoration, mode);
    } else if (command.starts_with("resize ")) {
      const auto size = harness::parseInts<int, 2>(command.substr(7), ' ');
      if (!size || (*size)[0] <= 0 || (*size)[1] <= 0) {
        state.running = false;
        return;
      }
      state.logicalWidth = (*size)[0];
      state.logicalHeight = (*size)[1];
      present(state);
    } else if (command.starts_with("title ")) {
      xdg_toplevel_set_title(state.toplevel, std::string(command.substr(6)).c_str());
    } else {
      std::println(stderr, "fractional-client: unknown decoration command {}", command);
      state.running = false;
    }
  }

  void readDecorationCommands(State& state) {
    char buffer[512];
    const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
    if (count <= 0) {
      state.running = false;
      return;
    }
    state.commands.append(buffer, static_cast<size_t>(count));
    size_t newline = 0;
    while ((newline = state.commands.find('\n')) != std::string::npos) {
      handleDecorationCommand(state, std::string_view(state.commands).substr(0, newline));
      state.commands.erase(0, newline + 1);
    }
  }

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
    auto& state = *static_cast<State*>(data);
    if (strcmp(interface, org_kde_kwin_server_decoration_manager_interface.name) == 0
        && std::getenv("LEGACY_DECORATION") != nullptr) {
      state.legacyManager = static_cast<org_kde_kwin_server_decoration_manager*>(
          wl_registry_bind(registry, name, &org_kde_kwin_server_decoration_manager_interface, 1)
      );
    } else if (
        strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0 && std::getenv("XDG_DECORATION") != nullptr
    ) {
      state.decorationManager = static_cast<zxdg_decoration_manager_v1*>(
          wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1)
      );
    } else if (strcmp(interface, wl_seat_interface.name) == 0 && std::getenv("LOG_POINTER") != nullptr) {
      auto* seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
      wl_seat_add_listener(seat, &kSeatListener, &state);
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (
        strcmp(interface, wl_subcompositor_interface.name) == 0 && std::getenv("UNMAPPED_SUBSURFACE") != nullptr
    ) {
      state.subcompositor =
          static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
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
    state.defaultWidth = std::stoi(argv[2]);
    state.defaultHeight = std::stoi(argv[3]);
  }
  if (argc > 4) {
    state.margin = std::stoi(argv[4]);
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
  if (std::getenv("UNMAPPED_SUBSURFACE") != nullptr) {
    if (state.subcompositor == nullptr) {
      std::println(stderr, "fractional-client: subcompositor unavailable");
      return 1;
    }
    wl_surface* auxiliary = wl_compositor_create_surface(state.compositor);
    wl_subsurface* subsurface = wl_subcompositor_get_subsurface(state.subcompositor, auxiliary, state.surface);
    wl_subsurface_set_position(subsurface, -200, -200);
  }
  state.viewport = wp_viewporter_get_viewport(state.viewporter, state.surface);
  wp_fractional_scale_v1* fractionalScale =
      wp_fractional_scale_manager_v1_get_fractional_scale(state.scaleManager, state.surface);
  wp_fractional_scale_v1_add_listener(fractionalScale, &kFractionalScaleListener, &state);

  // Legacy clients can select decorations before assigning the xdg role.
  if (const char* mode = std::getenv("LEGACY_DECORATION"); mode != nullptr) {
    if (state.legacyManager == nullptr) {
      std::println(stderr, "fractional-client: legacy decoration manager unavailable");
      return 1;
    }
    state.legacyDecoration = org_kde_kwin_server_decoration_manager_create(state.legacyManager, state.surface);
    org_kde_kwin_server_decoration_add_listener(state.legacyDecoration, &kLegacyListener, &state);
    org_kde_kwin_server_decoration_request_mode(state.legacyDecoration, strcmp(mode, "server") == 0 ? 2 : 1);
    if (wl_display_roundtrip(state.display) < 0)
      return 1;
  }

  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgSurfaceListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, title);
  if (const char* mode = std::getenv("XDG_DECORATION"); mode != nullptr) {
    if (state.decorationManager == nullptr) {
      std::println(stderr, "fractional-client: decoration manager unavailable");
      return 1;
    }
    state.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(state.decorationManager, state.toplevel);
    zxdg_toplevel_decoration_v1_add_listener(state.decoration, &kDecorationListener, &state);
    if (strcmp(mode, "unset") != 0) {
      zxdg_toplevel_decoration_v1_set_mode(
          state.decoration,
          strcmp(mode, "server") == 0 ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                      : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
      );
    }
  }
  wl_surface_commit(state.surface);

  const bool controlled = std::getenv("DECORATION_CONTROL") != nullptr;
  while (state.running) {
    if (!controlled) {
      if (wl_display_dispatch(state.display) < 0)
        break;
      continue;
    }
    if (wl_display_dispatch_pending(state.display) < 0 || wl_display_flush(state.display) < 0)
      break;
    pollfd fds[]{{wl_display_get_fd(state.display), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    if (poll(fds, 2, -1) < 0)
      break;
    if ((fds[0].revents & POLLIN) != 0 && wl_display_dispatch(state.display) < 0)
      break;
    if ((fds[1].revents & (POLLIN | POLLHUP)) != 0)
      readDecorationCommands(state);
  }
  wl_display_disconnect(state.display);
  return 0;
}
