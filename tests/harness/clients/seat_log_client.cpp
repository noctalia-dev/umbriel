// Maps a plain xdg toplevel and logs every seat input event it receives, so
// checks can assert which keys and buttons reach a focused surface.

#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <print>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {
  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_buffer* buffer = nullptr;
    void* pixels = MAP_FAILED;
    size_t size = 0;
    // Until the compositor configures a size, ask for something wide enough
    // that a tiled column is not narrower than the layout's own choice.
    int width = 640;
    int height = 480;
    // A configure asked for a size the current buffer does not have.
    bool resizePending = true;
  };

  const char* keyStateName(uint32_t value) { return value == WL_KEYBOARD_KEY_STATE_PRESSED ? "pressed" : "released"; }

  const char* buttonStateName(uint32_t value) {
    return value == WL_POINTER_BUTTON_STATE_PRESSED ? "pressed" : "released";
  }

  void keyboardKeymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); }

  void keyboardEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array* keys) {
    const size_t count = keys->size / sizeof(uint32_t);
    std::string line = std::format("keyboard-enter keys={}", count);
    const auto* codes = static_cast<const uint32_t*>(keys->data);
    for (size_t index = 0; index < count; ++index) {
      line += std::format(" code={}", codes[index]);
    }
    std::println("{}", line);
  }

  void keyboardLeave(void*, wl_keyboard*, uint32_t, wl_surface*) { std::println("keyboard-leave"); }

  void keyboardKey(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t keyState) {
    std::println("keyboard-key code={} state={}", key, keyStateName(keyState));
  }

  void keyboardModifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
  void keyboardRepeatInfo(void*, wl_keyboard*, int32_t, int32_t) {}

  constexpr wl_keyboard_listener kKeyboardListener = {
      .keymap = keyboardKeymap,
      .enter = keyboardEnter,
      .leave = keyboardLeave,
      .key = keyboardKey,
      .modifiers = keyboardModifiers,
      .repeat_info = keyboardRepeatInfo,
  };

  void pointerEnter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {
    std::println("pointer-enter");
  }

  void pointerLeave(void*, wl_pointer*, uint32_t, wl_surface*) { std::println("pointer-leave"); }

  // Motion is deliberately silent: a single pointer move floods the log the
  // checks parse.
  void pointerMotion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}

  void pointerButton(void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t buttonState) {
    std::println("pointer-button code={} state={}", button, buttonStateName(buttonState));
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

  // The harness creates and destroys virtual input devices while the client
  // runs, so capabilities appear and disappear rather than staying constant.
  void seatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) {
    auto& state = *static_cast<State*>(data);
    const bool hasPointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (hasPointer && state.pointer == nullptr) {
      state.pointer = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(state.pointer, &kPointerListener, &state);
    } else if (!hasPointer && state.pointer != nullptr) {
      wl_pointer_release(state.pointer);
      state.pointer = nullptr;
    }
    const bool hasKeyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    if (hasKeyboard && state.keyboard == nullptr) {
      state.keyboard = wl_seat_get_keyboard(seat);
      wl_keyboard_add_listener(state.keyboard, &kKeyboardListener, &state);
    } else if (!hasKeyboard && state.keyboard != nullptr) {
      wl_keyboard_release(state.keyboard);
      state.keyboard = nullptr;
    }
  }

  void seatName(void*, wl_seat*, const char*) {}

  constexpr wl_seat_listener kSeatListener = {
      .capabilities = seatCapabilities,
      .name = seatName,
  };

  // Allocates the buffer for the current configured size, replacing any
  // previous one.
  bool createBuffer(State& state) {
    const int stride = state.width * 4;
    const size_t size = static_cast<size_t>(stride) * static_cast<size_t>(state.height);
    const int fd = memfd_create("umbriel-seat-log-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) < 0) {
      return false;
    }
    void* pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
      close(fd);
      return false;
    }
    std::fill_n(static_cast<uint32_t*>(pixels), size / sizeof(uint32_t), 0xFF3388CC);
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(size));
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, state.width, state.height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (buffer == nullptr) {
      munmap(pixels, size);
      return false;
    }
    if (state.buffer != nullptr) {
      wl_buffer_destroy(state.buffer);
    }
    if (state.pixels != MAP_FAILED) {
      munmap(state.pixels, state.size);
    }
    state.buffer = buffer;
    state.pixels = pixels;
    state.size = size;
    return true;
  }

  void wmBasePing(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
  constexpr xdg_wm_base_listener kWmBaseListener = {.ping = wmBasePing};

  void xdgConfigure(void* data, xdg_surface* surface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(surface, serial);
    if (state.resizePending && !createBuffer(state)) {
      return;
    }
    state.resizePending = false;
    wl_surface_attach(state.surface, state.buffer, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, state.width, state.height);
    wl_surface_commit(state.surface);
  }
  constexpr xdg_surface_listener kXdgListener = {.configure = xdgConfigure};

  // The window is tiled, so the buffer follows the size the compositor asks
  // for. A client that keeps its own size is placed inside the tile instead,
  // which puts its surface somewhere the checks cannot compute.
  void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
    auto& state = *static_cast<State*>(data);
    if (width <= 0 || height <= 0 || (width == state.width && height == state.height)) {
      return;
    }
    state.width = width;
    state.height = height;
    state.resizePending = true;
  }
  void toplevelClose(void*, xdg_toplevel*) {}
  void toplevelConfigureBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
  void toplevelWmCapabilities(void*, xdg_toplevel*, wl_array*) {}
  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = toplevelConfigureBounds,
      .wm_capabilities = toplevelWmCapabilities,
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
      state.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    }
  }
  void registryRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryRemove};

} // namespace

int main(int argc, char** argv) {
  // Checks tail this log while the client keeps running, so a full stdio buffer
  // would hide events until exit.
  setvbuf(stdout, nullptr, _IOLBF, 0);
  const char* title = argc > 1 ? argv[1] : "seat-log-client";

  State state;
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "seat-log-client: cannot connect to a Wayland display");
    return EXIT_FAILURE;
  }
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  if (state.compositor == nullptr || state.shm == nullptr || state.seat == nullptr || state.wmBase == nullptr) {
    std::println(stderr, "seat-log-client: missing required Wayland globals");
    return EXIT_FAILURE;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, title);
  wl_surface_commit(state.surface);

  while (wl_display_dispatch(state.display) >= 0) {
  }
  return EXIT_SUCCESS;
}
