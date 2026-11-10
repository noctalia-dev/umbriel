// Maps an XDG toplevel and creates a grabbed popup on its first left click. The
// harness can then request redundant focus for the toplevel and observe whether
// the compositor incorrectly dismisses the popup. With --popup-on-stdin it
// instead creates an ungrabbed popup when a line arrives on stdin, so a check
// can open one while the toplevel is not shown. With --unmap-on-close it unmaps
// the toplevel on a close request and keeps its popup, so a check can unmap a
// window while its popup holds the keyboard grab. On the popup-on-stdin path an
// `unmap` line drops the popup buffer, so a check can take a popup away without
// destroying it, and a `sync` line answers with a `frame` once the compositor
// has read everything sent before it. FILL_COLOR, POPUP_FILL_COLOR, POPUP_WIDTH
// and POPUP_HEIGHT override the painted colours and the popup size.

#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <poll.h>
#include <print>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {

  constexpr int kWindowWidth = 1260;
  constexpr int kWindowHeight = 700;
  constexpr int kPopupWidth = 120;
  constexpr int kPopupHeight = 80;
  constexpr uint32_t kWindowFill = 0xFF5577AA;
  constexpr uint32_t kPopupFill = 0xFFEEAA33;
  constexpr uint32_t kLeftButton = 0x110;

  struct Buffer {
    wl_buffer* resource = nullptr;
    void* pixels = MAP_FAILED;
    size_t size = 0;
  };

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_surface* popupSurface = nullptr;
    xdg_surface* popupXdgSurface = nullptr;
    xdg_popup* popup = nullptr;
    Buffer windowBuffer;
    Buffer popupBuffer;
    int popupWidth = kPopupWidth;
    int popupHeight = kPopupHeight;
    uint32_t windowFill = kWindowFill;
    uint32_t popupFill = kPopupFill;
    bool mapped = false;
    bool popupWanted = false;
    bool popupMapped = false;
    bool unmapOnClose = false;
  };

  Buffer createBuffer(State& state, int width, int height, uint32_t color) {
    Buffer buffer;
    const int stride = width * 4;
    buffer.size = static_cast<size_t>(stride * height);
    const int fd = memfd_create("umbriel-popup-client", MFD_CLOEXEC);
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
    buffer.resource = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
  }

  void attachPopupBuffer(State& state) {
    // Toolkits set an explicit window geometry, which outlives the surface's unmap; without one the compositor
    // derives the box from the buffer and forgets it with the buffer.
    xdg_surface_set_window_geometry(state.popupXdgSurface, 0, 0, state.popupWidth, state.popupHeight);
    wl_surface_attach(state.popupSurface, state.popupBuffer.resource, 0, 0);
    wl_surface_damage_buffer(state.popupSurface, 0, 0, state.popupWidth, state.popupHeight);
    wl_surface_commit(state.popupSurface);
    state.popupMapped = true;
    std::println("popup-mapped");
    std::fflush(stdout);
  }

  void unmapPopup(State& state) {
    state.popupWanted = false;
    if (!state.popupMapped) {
      return;
    }
    // Dropping the buffer unmaps the surface but keeps the popup object, like a client hiding a menu.
    wl_surface_attach(state.popupSurface, nullptr, 0, 0);
    wl_surface_commit(state.popupSurface);
    state.popupMapped = false;
    std::println("popup-unmapped");
    std::fflush(stdout);
  }

  void popupSurfaceConfigure(void* data, xdg_surface* xdgSurface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(xdgSurface, serial);
    // A popup that was never configured must not draw yet; its configure ack maps it.
    if (!state.popupWanted || state.popupMapped) {
      return;
    }

    attachPopupBuffer(state);
  }

  constexpr xdg_surface_listener kPopupXdgSurfaceListener = {
      .configure = popupSurfaceConfigure,
  };

  void frameDone(void*, wl_callback* callback, uint32_t) {
    wl_callback_destroy(callback);
    std::println("frame");
    std::fflush(stdout);
  }

  constexpr wl_callback_listener kFrameListener = {
      .done = frameDone,
  };

  // A commit is answered only when the compositor reads it, and nothing in it sends a reply. Requesting a frame on
  // the toplevel behind it does: the callback arrives on the next drawn frame, after the compositor has read both.
  void syncFrame(State& state) {
    wl_callback* callback = wl_surface_frame(state.surface);
    wl_callback_add_listener(callback, &kFrameListener, nullptr);
    wl_surface_commit(state.surface);
  }

  void popupConfigure(void*, xdg_popup*, int32_t, int32_t, int32_t, int32_t) {}

  void popupDone(void*, xdg_popup*) {
    std::println("popup-done");
    std::fflush(stdout);
  }

  void popupRepositioned(void*, xdg_popup*, uint32_t) {}

  constexpr xdg_popup_listener kPopupListener = {
      .configure = popupConfigure,
      .popup_done = popupDone,
      .repositioned = popupRepositioned,
  };

  void createPopup(State& state, std::optional<uint32_t> grabSerial) {
    state.popupSurface = wl_compositor_create_surface(state.compositor);
    state.popupXdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.popupSurface);
    xdg_surface_add_listener(state.popupXdgSurface, &kPopupXdgSurfaceListener, &state);

    xdg_positioner* positioner = xdg_wm_base_create_positioner(state.wmBase);
    xdg_positioner_set_size(positioner, state.popupWidth, state.popupHeight);
    xdg_positioner_set_anchor_rect(positioner, 0, 0, 1, 1);
    xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    xdg_positioner_set_offset(positioner, 20, 20);

    state.popup = xdg_surface_get_popup(state.popupXdgSurface, state.xdgSurface, positioner);
    xdg_positioner_destroy(positioner);
    xdg_popup_add_listener(state.popup, &kPopupListener, &state);
    if (grabSerial) {
      xdg_popup_grab(state.popup, state.seat, *grabSerial);
    }
    state.popupWanted = true;
    wl_surface_commit(state.popupSurface);
  }

  void pointerEnter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {
    std::println("pointer-enter");
    std::fflush(stdout);
  }
  void pointerLeave(void*, wl_pointer*, uint32_t, wl_surface*) {}
  void pointerMotion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}

  void pointerButton(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t buttonState) {
    auto& state = *static_cast<State*>(data);
    if (button == kLeftButton && buttonState == WL_POINTER_BUTTON_STATE_PRESSED && state.popup == nullptr) {
      createPopup(state, serial);
      std::println("popup-requested");
      std::fflush(stdout);
    }
  }

  void pointerAxis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
  void pointerFrame(void*, wl_pointer*) {}
  void pointerAxisSource(void*, wl_pointer*, uint32_t) {}
  void pointerAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
  void pointerAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}
  void pointerAxisValue120(void*, wl_pointer*, uint32_t, int32_t) {}
  void pointerAxisRelativeDirection(void*, wl_pointer*, uint32_t, uint32_t) {}
#ifdef WL_POINTER_WARP_SINCE_VERSION
  void pointerWarp(void*, wl_pointer*, wl_fixed_t, wl_fixed_t) {}
#endif

  constexpr wl_pointer_listener kPointerListener = {
      .enter = pointerEnter,
      .leave = pointerLeave,
      .motion = pointerMotion,
      .button = pointerButton,
      .axis = pointerAxis,
      .frame = pointerFrame,
      .axis_source = pointerAxisSource,
      .axis_stop = pointerAxisStop,
      .axis_discrete = pointerAxisDiscrete,
      .axis_value120 = pointerAxisValue120,
      .axis_relative_direction = pointerAxisRelativeDirection,
#ifdef WL_POINTER_WARP_SINCE_VERSION
      .warp = pointerWarp,
#endif
  };

  void seatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) {
    auto& state = *static_cast<State*>(data);
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && state.pointer == nullptr) {
      state.pointer = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(state.pointer, &kPointerListener, &state);
    }
  }

  void seatName(void*, wl_seat*, const char*) {}

  constexpr wl_seat_listener kSeatListener = {
      .capabilities = seatCapabilities,
      .name = seatName,
  };

  void surfaceConfigure(void* data, xdg_surface* xdgSurface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(xdgSurface, serial);
    if (!state.mapped) {
      state.mapped = true;
      wl_surface_attach(state.surface, state.windowBuffer.resource, 0, 0);
      wl_surface_damage_buffer(state.surface, 0, 0, kWindowWidth, kWindowHeight);
    }
    // An ack alone leaves the compositor's configure serial unanswered, so a check that waits for a settled instance
    // would never see one. Committing re-uses the current buffer, which is what a client that ignores the size does.
    wl_surface_commit(state.surface);
  }

  constexpr xdg_surface_listener kXdgSurfaceListener = {
      .configure = surfaceConfigure,
  };

  void toplevelConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}
  void toplevelClose(void* data, xdg_toplevel*) {
    auto& state = *static_cast<State*>(data);
    if (!state.unmapOnClose) {
      return;
    }
    wl_surface_attach(state.surface, nullptr, 0, 0);
    wl_surface_commit(state.surface);
    std::println("unmapped");
    std::fflush(stdout);
  }

  // The shell is bound at up to version 6, which sends these too; this client has no use for them.
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

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      state.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5U)));
      wl_seat_add_listener(state.seat, &kSeatListener, &state);
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
      state.wmBase =
          static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 6U)));
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    }
  }

  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };

  void destroyBuffer(Buffer& buffer) {
    if (buffer.resource != nullptr) {
      wl_buffer_destroy(buffer.resource);
    }
    if (buffer.pixels != MAP_FAILED) {
      munmap(buffer.pixels, buffer.size);
    }
  }

  uint32_t envColor(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : fallback;
  }

  int envSize(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
      return fallback;
    }
    const long parsed = std::strtol(value, nullptr, 10);
    return parsed > 0 && parsed <= 4096 ? static_cast<int>(parsed) : fallback;
  }

} // namespace

int main(int argc, char** argv) {
  State state;
  const bool popupOnStdin = argc > 1 && std::strcmp(argv[1], "--popup-on-stdin") == 0;
  state.unmapOnClose = argc > 1 && std::strcmp(argv[1], "--unmap-on-close") == 0;
  state.popupWidth = envSize("POPUP_WIDTH", kPopupWidth);
  state.popupHeight = envSize("POPUP_HEIGHT", kPopupHeight);
  state.windowFill = envColor("FILL_COLOR", kWindowFill);
  state.popupFill = envColor("POPUP_FILL_COLOR", kPopupFill);
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "popup-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }

  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  wl_display_roundtrip(state.display);

  if (state.compositor == nullptr
      || state.shm == nullptr
      || state.seat == nullptr
      || state.pointer == nullptr
      || state.wmBase == nullptr) {
    std::println(stderr, "popup-client: compositor is missing a required Wayland global");
    return EXIT_FAILURE;
  }

  state.windowBuffer = createBuffer(state, kWindowWidth, kWindowHeight, state.windowFill);
  state.popupBuffer = createBuffer(state, state.popupWidth, state.popupHeight, state.popupFill);
  if (state.windowBuffer.resource == nullptr || state.popupBuffer.resource == nullptr) {
    std::println(stderr, "popup-client: failed to allocate shared-memory buffers");
    return EXIT_FAILURE;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgSurfaceListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, "popup-focus-regression");
  wl_surface_commit(state.surface);

  while (!state.mapped && wl_display_dispatch(state.display) >= 0) {
  }
  if (!state.mapped || wl_display_roundtrip(state.display) < 0) {
    return EXIT_FAILURE;
  }
  std::println("ready");
  std::fflush(stdout);

  if (!popupOnStdin) {
    while (wl_display_dispatch(state.display) >= 0) {
    }
  } else {
    pollfd fds[] = {
        {.fd = wl_display_get_fd(state.display), .events = POLLIN, .revents = 0},
        {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
    };
    while (wl_display_dispatch_pending(state.display) >= 0 && wl_display_flush(state.display) >= 0) {
      if (poll(fds, std::size(fds), -1) < 0) {
        break;
      }
      if ((fds[0].revents & POLLIN) != 0 && wl_display_dispatch(state.display) < 0) {
        break;
      }
      if ((fds[1].revents & (POLLIN | POLLHUP)) != 0) {
        char line[64] = {};
        const ssize_t length = read(STDIN_FILENO, line, sizeof(line) - 1);
        if (length <= 0) {
          fds[1].fd = -1;
          continue;
        }
        char* command = line;
        while (*command == ' ' || *command == '\t') {
          ++command;
        }
        if (std::strncmp(command, "unmap", 5) == 0) {
          unmapPopup(state);
        } else if (std::strncmp(command, "sync", 4) == 0) {
          syncFrame(state);
        } else if (state.popup == nullptr) {
          createPopup(state, std::nullopt);
          std::println("popup-requested");
          std::fflush(stdout);
        }
      }
    }
  }

  destroyBuffer(state.popupBuffer);
  destroyBuffer(state.windowBuffer);
  wl_display_disconnect(state.display);
  return EXIT_SUCCESS;
}
