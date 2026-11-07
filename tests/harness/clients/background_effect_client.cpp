// Maps a translucent xdg toplevel that requests background blur through
// ext_background_effect_v1. Prints "capabilities <flags>" when the manager is
// bound, "bound" when the surface object is created, and "mapped <w>x<h>" once
// the toplevel is up. Reads commands from stdin:
//   r <x1> <y1> <x2> <y2>  set that surface-local blur region and commit
//   n                      clear the region (NULL) and commit
//   d                      destroy the blur object without committing
//   p                      commit without changing the region
//   q                      disconnect
// `--probe` binds the manager instead of mapping a window, prints its capabilities, and exits.
// Every applied command prints "region <x1> <y1> <x2> <y2>" or "region none"
// so a check can wait for the commit instead of sleeping.
// FILL_COLOR=<ARGB> paints the toplevel; the default is half-transparent black
// so a backdrop blur underneath is visible. Usage: background-effect-client
// [title [width height]].
#include "ext-background-effect-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <poll.h>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    ext_background_effect_manager_v1* blurManager = nullptr;
    ext_background_effect_surface_v1* blur = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_buffer* buffer = nullptr;
    uint32_t fill = 0x80000000U;
    int width = 500;
    int height = 520;
    int configuredWidth = 0;
    int configuredHeight = 0;
    bool configured = false;
    bool running = true;
  };

  bool present(State& state) {
    if (!state.configured) {
      return true;
    }
    const int width = state.width;
    const int height = state.height;
    const size_t stride = static_cast<size_t>(width) * 4;
    const size_t size = stride * static_cast<size_t>(height);
    const int fd = memfd_create("umbriel-background-effect-client", MFD_CLOEXEC);
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
    for (size_t i = 0; i < size / 4; ++i) {
      words[i] = state.fill;
    }
    munmap(pixels, size);

    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(size));
    if (state.buffer != nullptr) {
      wl_buffer_destroy(state.buffer);
    }
    state.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, static_cast<int>(stride), WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (state.buffer == nullptr) {
      return false;
    }
    wl_surface_attach(state.surface, state.buffer, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, width, height);
    wl_surface_commit(state.surface);
    std::println("mapped {}x{}", width, height);
    std::fflush(stdout);
    return true;
  }

  void xdgSurfaceConfigure(void* data, xdg_surface* xdgSurface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(xdgSurface, serial);
    // Follow the compositor's size, so a window rule's floating size is the surface size the check reasons about.
    if (state.configuredWidth > 0) {
      state.width = state.configuredWidth;
    }
    if (state.configuredHeight > 0) {
      state.height = state.configuredHeight;
    }
    state.configured = true;
    if (!present(state)) {
      std::println(stderr, "background-effect-client: could not present");
    }
  }

  constexpr xdg_surface_listener kXdgSurfaceListener = {
      .configure = xdgSurfaceConfigure,
  };

  void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
    auto& state = *static_cast<State*>(data);
    state.configuredWidth = width;
    state.configuredHeight = height;
  }
  void toplevelClose(void* data, xdg_toplevel*) { static_cast<State*>(data)->running = false; }
  void toplevelConfigureBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
  void toplevelWmCapabilities(void*, xdg_toplevel*, wl_array*) {}

  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = toplevelConfigureBounds,
      .wm_capabilities = toplevelWmCapabilities,
  };

  void wmBasePing(void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }

  constexpr xdg_wm_base_listener kWmBaseListener = {
      .ping = wmBasePing,
  };

  void managerCapabilities(void*, ext_background_effect_manager_v1*, uint32_t flags) {
    std::println("capabilities {}", flags);
    std::fflush(stdout);
  }

  constexpr ext_background_effect_manager_v1_listener kManagerListener = {
      .capabilities = managerCapabilities,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
      state.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    } else if (std::strcmp(interface, ext_background_effect_manager_v1_interface.name) == 0) {
      state.blurManager = static_cast<ext_background_effect_manager_v1*>(
          wl_registry_bind(registry, name, &ext_background_effect_manager_v1_interface, std::min(version, 1U))
      );
    }
  }

  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };

  std::optional<int> asInt(std::string_view text) {
    std::istringstream stream{std::string(text)};
    int value = 0;
    if (!(stream >> value)) {
      return std::nullopt;
    }
    return value;
  }

  bool applyRegion(State& state, const std::string& line) {
    if (line == "n") {
      ext_background_effect_surface_v1_set_blur_region(state.blur, nullptr);
      wl_surface_commit(state.surface);
      std::println("region none");
      std::fflush(stdout);
      return true;
    }
    if (line == "p") {
      wl_surface_commit(state.surface);
      std::println("committed");
      std::fflush(stdout);
      return true;
    }
    if (line == "d") {
      if (state.blur != nullptr) {
        ext_background_effect_surface_v1_destroy(state.blur);
        state.blur = nullptr;
      }
      std::println("destroyed");
      std::fflush(stdout);
      return true;
    }
    if (state.blur == nullptr) {
      std::println(stderr, "background-effect-client: the blur object is gone");
      return false;
    }
    if (line.starts_with("r ")) {
      std::istringstream stream{line.substr(2)};
      int x1 = 0;
      int y1 = 0;
      int x2 = 0;
      int y2 = 0;
      if (!(stream >> x1 >> y1 >> x2 >> y2)) {
        std::println(stderr, "background-effect-client: bad region '{}'", line);
        return false;
      }
      wl_region* region = wl_compositor_create_region(state.compositor);
      wl_region_add(region, x1, y1, x2 - x1, y2 - y1);
      ext_background_effect_surface_v1_set_blur_region(state.blur, region);
      wl_region_destroy(region);
      wl_surface_commit(state.surface);
      std::println("region {} {} {} {}", x1, y1, x2, y2);
      std::fflush(stdout);
      return true;
    }
    return true;
  }

} // namespace

int main(int argc, char** argv) {
  State state;
  const bool probe = argc > 1 && std::string(argv[1]) == "--probe";
  const char* title = !probe && argc > 1 ? argv[1] : "background-effect";
  if (argc > 3) {
    if (const auto width = asInt(argv[2]); width.has_value() && *width > 0) {
      state.width = *width;
    }
    if (const auto height = asInt(argv[3]); height.has_value() && *height > 0) {
      state.height = *height;
    }
  }
  if (const char* fill = std::getenv("FILL_COLOR")) {
    state.fill = static_cast<uint32_t>(std::strtoul(fill, nullptr, 0));
  }

  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "background-effect-client: no display");
    return 1;
  }
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  if (state.compositor == nullptr || state.shm == nullptr || state.wmBase == nullptr) {
    std::println(stderr, "background-effect-client: missing core globals");
    return 1;
  }
  if (state.blurManager == nullptr) {
    std::println(stderr, "background-effect-client: ext_background_effect_manager_v1 is not advertised");
    return 1;
  }
  ext_background_effect_manager_v1_add_listener(state.blurManager, &kManagerListener, &state);
  if (probe) {
    // A roundtrip delivers the capabilities event the manager sends on bind.
    wl_display_roundtrip(state.display);
    wl_display_disconnect(state.display);
    return 0;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  state.blur = ext_background_effect_manager_v1_get_background_effect(state.blurManager, state.surface);
  std::println("bound");
  std::fflush(stdout);

  xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgSurfaceListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, title);
  wl_surface_commit(state.surface);

  while (state.running) {
    if (wl_display_dispatch_pending(state.display) < 0 || wl_display_flush(state.display) < 0) {
      break;
    }
    pollfd fds[]{{wl_display_get_fd(state.display), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    if (poll(fds, 2, -1) < 0) {
      break;
    }
    if ((fds[0].revents & POLLIN) != 0 && wl_display_dispatch(state.display) < 0) {
      break;
    }
    if ((fds[1].revents & (POLLIN | POLLHUP)) != 0) {
      std::string line;
      if (std::getline(std::cin, line) && !line.empty()) {
        if (line == "q") {
          break;
        }
        applyRegion(state, line);
      } else {
        break;
      }
    }
  }

  if (state.blur != nullptr) {
    ext_background_effect_surface_v1_destroy(state.blur);
  }
  wl_display_disconnect(state.display);
  return 0;
}
