// Maps a top exclusive zone, a full-output background layer when the height is zero, or a 200x200 bottom-layer or
// red overlay-layer square. It stays mapped until that output closes the layer surface. `keyboard=none|on-demand|
// exclusive` picks the layer surface's keyboard interactivity, and every keyboard enter and leave the surface receives
// is logged. `release-on-escape` drops the interactivity to none when Escape is released. `popup-on-click` behaves like
// a bar opening a menu: the first left press switches the layer to on-demand and opens a grabbing xdg_popup.
// `popup-on-release` batches the release until the pointer frame and anchors its menu at the last valid position.
// LAYER_CONTROL enables stdin commands: frames LABEL COUNT, color LABEL AARRGGBB, child-color LABEL AARRGGBB,
// child-move LABEL X Y, and margins LABEL TOP RIGHT BOTTOM LEFT. Each prints done after its frame callbacks arrive.

#include <wayland-client.h>

#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <print>
#include <sstream>
#include <string>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {
  struct Output {
    wl_output* resource = nullptr;
    std::string name;
  };

  struct Buffer {
    wl_buffer* resource = nullptr;
    void* pixels = MAP_FAILED;
    size_t size = 0;
    int width = 0;
    int height = 0;
  };

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    zwlr_layer_shell_v1* layerShell = nullptr;
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_pointer* pointer = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wl_surface* popupSurface = nullptr;
    xdg_surface* popupXdgSurface = nullptr;
    xdg_popup* popup = nullptr;
    Buffer popupBuffer;
    bool popupMapped = false;
    bool popupOnClick = false;
    bool popupOnRelease = false;
    bool hasPointerPosition = false;
    int pointerX = 0;
    int pointerY = 0;
    uint32_t pendingReleaseSerial = 0;
    std::vector<std::unique_ptr<Output>> outputs;
    wl_surface* surface = nullptr;
    zwlr_layer_surface_v1* layerSurface = nullptr;
    wl_callback* frame = nullptr;
    Buffer buffer;
    wl_surface* childSurface = nullptr;
    wl_subsurface* childSubsurface = nullptr;
    Buffer childBuffer;
    wl_surface* frameSurface = nullptr;
    std::string commands;
    std::string commandLabel;
    int remainingFrames = 0;
    int requestedFrames = 0;
    bool ready = false;
    bool closed = false;
    bool failed = false;
    uint32_t fillColor = 0xFF202020;
    bool logConfigures = false;
    bool releaseOnEscape = false;
    uint32_t keyboardInteractivity = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
  };

  void
  outputGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
  void outputMode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
  void outputDone(void*, wl_output*) {}
  void outputScale(void*, wl_output*, int32_t) {}
  void outputName(void* data, wl_output*, const char* name) {
    static_cast<Output*>(data)->name = name != nullptr ? name : "";
  }
  void outputDescription(void*, wl_output*, const char*) {}

  constexpr wl_output_listener kOutputListener = {
      .geometry = outputGeometry,
      .mode = outputMode,
      .done = outputDone,
      .scale = outputScale,
      .name = outputName,
      .description = outputDescription,
  };

  Buffer createBuffer(State& state, int width, int height) {
    Buffer buffer{.width = width, .height = height};
    const int stride = width * 4;
    buffer.size = static_cast<size_t>(stride * height);
    const int fd = memfd_create("umbriel-layer-client", MFD_CLOEXEC);
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
    std::fill_n(static_cast<uint32_t*>(buffer.pixels), buffer.size / sizeof(uint32_t), state.fillColor);
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(buffer.size));
    buffer.resource = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
  }

  void destroyBuffer(Buffer& buffer) {
    if (buffer.resource != nullptr) {
      wl_buffer_destroy(buffer.resource);
    }
    if (buffer.pixels != MAP_FAILED) {
      munmap(buffer.pixels, buffer.size);
    }
    buffer = {};
  }

  void requestFrame(State& state);

  void frameDone(void* data, wl_callback* callback, uint32_t) {
    auto& state = *static_cast<State*>(data);
    wl_callback_destroy(callback);
    state.frame = nullptr;
    if (!state.ready) {
      state.ready = true;
      std::println("ready");
      std::fflush(stdout);
    }
    if (state.remainingFrames > 0) {
      if (--state.remainingFrames > 0) {
        requestFrame(state);
      } else {
        std::println("done {} callbacks {}", state.commandLabel, state.requestedFrames);
        std::fflush(stdout);
      }
    }
  }

  constexpr wl_callback_listener kFrameListener = {.done = frameDone};

  void requestFrame(State& state) {
    state.frame = wl_surface_frame(state.frameSurface);
    wl_callback_add_listener(state.frame, &kFrameListener, &state);
    wl_surface_commit(state.frameSurface);
  }

  void handleCommand(State& state, const std::string& line) {
    std::istringstream command(line);
    std::string action;
    command >> action >> state.commandLabel;
    if (state.commandLabel.empty() || state.frame != nullptr) {
      state.failed = state.closed = true;
      std::println(stderr, "layer-client: invalid or overlapping command '{}'", line);
      return;
    }
    state.frameSurface = state.surface;
    state.requestedFrames = 1;
    if (action == "frames") {
      command >> state.requestedFrames;
      if (state.requestedFrames <= 0) {
        command.setstate(std::ios::failbit);
      }
    } else if (action == "color" || action == "child-color") {
      uint32_t color = 0;
      command >> std::hex >> color;
      if (command.fail()) {
        state.failed = state.closed = true;
        return;
      }
      const bool child = action == "child-color";
      if (child && state.childSurface == nullptr) {
        if (state.subcompositor == nullptr) {
          state.failed = state.closed = true;
          return;
        }
        state.childSurface = wl_compositor_create_surface(state.compositor);
        state.childSubsurface = wl_subcompositor_get_subsurface(state.subcompositor, state.childSurface, state.surface);
        wl_subsurface_set_desync(state.childSubsurface);
        wl_surface_commit(state.surface);
      }
      const uint32_t parentColor = state.fillColor;
      state.fillColor = color;
      Buffer replacement = createBuffer(state, child ? 200 : state.buffer.width, child ? 200 : state.buffer.height);
      if (child) {
        state.fillColor = parentColor;
      }
      if (replacement.resource == nullptr) {
        state.failed = state.closed = true;
        return;
      }
      state.frameSurface = child ? state.childSurface : state.surface;
      wl_surface_attach(state.frameSurface, replacement.resource, 0, 0);
      wl_surface_damage_buffer(state.frameSurface, 0, 0, replacement.width, replacement.height);
      Buffer& previous = child ? state.childBuffer : state.buffer;
      destroyBuffer(previous);
      previous = replacement;
    } else if (action == "child-move") {
      int x = 0;
      int y = 0;
      command >> x >> y;
      if (state.childSubsurface == nullptr) {
        command.setstate(std::ios::failbit);
      } else {
        wl_subsurface_set_position(state.childSubsurface, x, y);
      }
    } else if (action == "margins") {
      int top = 0;
      int right = 0;
      int bottom = 0;
      int left = 0;
      command >> top >> right >> bottom >> left;
      zwlr_layer_surface_v1_set_margin(state.layerSurface, top, right, bottom, left);
    } else {
      command.setstate(std::ios::failbit);
    }
    if (command.fail()) {
      state.failed = state.closed = true;
      std::println(stderr, "layer-client: invalid command '{}'", line);
      return;
    }
    state.remainingFrames = state.requestedFrames;
    requestFrame(state);
  }

  void readCommands(State& state) {
    char buffer[1024];
    const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
    if (count <= 0) {
      if (count == 0 || (errno != EINTR && errno != EAGAIN)) {
        state.closed = true;
      }
      return;
    }
    state.commands.append(buffer, static_cast<size_t>(count));
    size_t newline = 0;
    while ((newline = state.commands.find('\n')) != std::string::npos) {
      const std::string command = state.commands.substr(0, newline);
      state.commands.erase(0, newline + 1);
      handleCommand(state, command);
      if (state.closed) {
        return;
      }
    }
  }

  void
  layerConfigure(void* data, zwlr_layer_surface_v1* layerSurface, uint32_t serial, uint32_t width, uint32_t height) {
    auto& state = *static_cast<State*>(data);
    zwlr_layer_surface_v1_ack_configure(layerSurface, serial);
    const int configuredWidth = std::max(1, static_cast<int>(width));
    const int configuredHeight = std::max(1, static_cast<int>(height));
    if (state.logConfigures) {
      std::println("configured-size={}x{}", configuredWidth, configuredHeight);
    }
    if (state.buffer.resource == nullptr
        || state.buffer.width != configuredWidth
        || state.buffer.height != configuredHeight) {
      destroyBuffer(state.buffer);
      state.buffer = createBuffer(state, configuredWidth, configuredHeight);
    }
    if (state.buffer.resource == nullptr) {
      state.failed = true;
      state.closed = true;
      return;
    }
    wl_surface_attach(state.surface, state.buffer.resource, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, configuredWidth, configuredHeight);
    if (!state.ready && state.frame == nullptr) {
      state.frame = wl_surface_frame(state.surface);
      wl_callback_add_listener(state.frame, &kFrameListener, &state);
    }
    wl_surface_commit(state.surface);
  }

  void layerClosed(void* data, zwlr_layer_surface_v1*) { static_cast<State*>(data)->closed = true; }

  constexpr zwlr_layer_surface_v1_listener kLayerSurfaceListener = {
      .configure = layerConfigure,
      .closed = layerClosed,
  };

  void keyboardKeymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); }
  void keyboardEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) { std::println("keyboard-enter"); }
  void keyboardLeave(void*, wl_keyboard*, uint32_t, wl_surface*) { std::println("keyboard-leave"); }
  void keyboardKey(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t keyState) {
    std::println("keyboard-key code={} state={}", key, keyState);
    auto& state = *static_cast<State*>(data);
    if (state.releaseOnEscape && key == 1 && keyState == WL_KEYBOARD_KEY_STATE_RELEASED) {
      zwlr_layer_surface_v1_set_keyboard_interactivity(
          state.layerSurface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE
      );
      wl_surface_commit(state.surface);
    }
  }
  void keyboardModifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
  void keyboardRepeatInfo(void*, wl_keyboard*, int32_t, int32_t) {}

  constexpr int kPopupSize = 80;

  void popupSurfaceConfigure(void* data, xdg_surface* xdgSurface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(xdgSurface, serial);
    if (state.popupMapped) {
      return;
    }
    state.popupMapped = true;
    wl_surface_attach(state.popupSurface, state.popupBuffer.resource, 0, 0);
    wl_surface_damage_buffer(state.popupSurface, 0, 0, kPopupSize, kPopupSize);
    wl_surface_commit(state.popupSurface);
    std::println("popup-mapped");
  }

  constexpr xdg_surface_listener kPopupXdgSurfaceListener = {.configure = popupSurfaceConfigure};

  void popupConfigure(void*, xdg_popup*, int32_t x, int32_t y, int32_t width, int32_t height) {
    std::println("popup-configure x={} y={} width={} height={}", x, y, width, height);
  }
  void popupDone(void*, xdg_popup*) { std::println("popup-done"); }
  void popupRepositioned(void*, xdg_popup*, uint32_t) {}

  constexpr xdg_popup_listener kPopupListener = {
      .configure = popupConfigure,
      .popup_done = popupDone,
      .repositioned = popupRepositioned,
  };

  // Mirrors a bar opening a menu: the layer surface only asks for the keyboard right before its popup grabs it.
  void openPopup(State& state, uint32_t serial, int anchorX = 0, int anchorY = 0) {
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        state.layerSurface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
    );
    wl_surface_commit(state.surface);

    state.popupSurface = wl_compositor_create_surface(state.compositor);
    state.popupXdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.popupSurface);
    xdg_surface_add_listener(state.popupXdgSurface, &kPopupXdgSurfaceListener, &state);
    xdg_positioner* positioner = xdg_wm_base_create_positioner(state.wmBase);
    xdg_positioner_set_size(positioner, kPopupSize, kPopupSize);
    xdg_positioner_set_anchor_rect(positioner, anchorX, anchorY, 1, 1);
    xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
    xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
    state.popup = xdg_surface_get_popup(state.popupXdgSurface, nullptr, positioner);
    xdg_positioner_destroy(positioner);
    xdg_popup_add_listener(state.popup, &kPopupListener, &state);
    zwlr_layer_surface_v1_get_popup(state.layerSurface, state.popup);
    xdg_popup_grab(state.popup, state.seat, serial);
    wl_surface_commit(state.popupSurface);
    std::println("popup-requested");
  }

  void pointerEnter(void* data, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
    auto& state = *static_cast<State*>(data);
    state.hasPointerPosition = true;
    state.pointerX = wl_fixed_to_int(x);
    state.pointerY = wl_fixed_to_int(y);
  }
  void pointerLeave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    static_cast<State*>(data)->hasPointerPosition = false;
  }
  void pointerMotion(void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    auto& state = *static_cast<State*>(data);
    state.hasPointerPosition = true;
    state.pointerX = wl_fixed_to_int(x);
    state.pointerY = wl_fixed_to_int(y);
  }
  void pointerButton(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t buttonState) {
    constexpr uint32_t kLeftButton = 0x110;
    auto& state = *static_cast<State*>(data);
    if (state.popupOnClick
        && button == kLeftButton
        && buttonState == WL_POINTER_BUTTON_STATE_PRESSED
        && state.popup == nullptr) {
      openPopup(state, serial);
    }
    if (state.popupOnRelease
        && button == kLeftButton
        && buttonState == WL_POINTER_BUTTON_STATE_RELEASED
        && state.popup == nullptr) {
      state.pendingReleaseSerial = serial;
    }
  }
  void pointerAxis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
  void pointerFrame(void* data, wl_pointer*) {
    auto& state = *static_cast<State*>(data);
    if (state.pendingReleaseSerial == 0) {
      return;
    }
    const int x = state.hasPointerPosition ? state.pointerX : 0;
    const int y = state.hasPointerPosition ? state.pointerY : 0;
    std::println("pointer-release-frame valid={} x={} y={}", state.hasPointerPosition, x, y);
    openPopup(state, state.pendingReleaseSerial, x, y);
    state.pendingReleaseSerial = 0;
  }
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

  void wmBasePing(void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }

  constexpr xdg_wm_base_listener kWmBaseListener = {.ping = wmBasePing};

  constexpr wl_keyboard_listener kKeyboardListener = {
      .keymap = keyboardKeymap,
      .enter = keyboardEnter,
      .leave = keyboardLeave,
      .key = keyboardKey,
      .modifiers = keyboardModifiers,
      .repeat_info = keyboardRepeatInfo,
  };

  // The harness creates and destroys virtual keyboards while the client runs, so the capability comes and goes.
  void seatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) {
    auto& state = *static_cast<State*>(data);
    const bool hasKeyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    if (hasKeyboard && state.keyboard == nullptr) {
      state.keyboard = wl_seat_get_keyboard(seat);
      wl_keyboard_add_listener(state.keyboard, &kKeyboardListener, &state);
    } else if (!hasKeyboard && state.keyboard != nullptr) {
      wl_keyboard_release(state.keyboard);
      state.keyboard = nullptr;
    }
    if ((state.popupOnClick || state.popupOnRelease)
        && (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0
        && state.pointer == nullptr) {
      state.pointer = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(state.pointer, &kPointerListener, &state);
    }
  }

  void seatName(void*, wl_seat*, const char*) {}

  constexpr wl_seat_listener kSeatListener = {
      .capabilities = seatCapabilities,
      .name = seatName,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, wl_subcompositor_interface.name) == 0) {
      state.subcompositor =
          static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      state.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5U)));
      wl_seat_add_listener(state.seat, &kSeatListener, &state);
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
      state.layerShell = static_cast<zwlr_layer_shell_v1*>(
          wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, std::min(version, 4U))
      );
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
      state.wmBase =
          static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 6U)));
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
      auto output = std::make_unique<Output>();
      output->resource =
          static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 4U)));
      wl_output_add_listener(output->resource, &kOutputListener, output.get());
      state.outputs.push_back(std::move(output));
    }
  }

  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };
} // namespace

int main(int argc, char** argv) {
  // Checks tail this log while the client keeps running, so a full stdio buffer would hide events until exit.
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc < 3) {
    std::println(
        "usage: layer-client <output> <exclusive-height-or-zero-background> [bottom-layer|overlay-layer] "
        "[log-configures] "
        "[keyboard=none|on-demand|exclusive] [release-on-escape] [popup-on-click|popup-on-release]"
    );
    return EXIT_FAILURE;
  }
  const std::string outputName = argv[1];
  const int exclusiveHeight = std::stoi(argv[2]);
  if (exclusiveHeight < 0) {
    std::println(stderr, "layer-client: exclusive height must not be negative");
    return EXIT_FAILURE;
  }

  State state;
  // A 200x200 bottom-layer square in the top-left corner, which the overview mirrors into every workspace preview.
  bool bottom = false;
  bool overlay = false;
  for (int index = 3; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "bottom-layer") {
      bottom = true;
    } else if (option == "overlay-layer") {
      overlay = true;
    } else if (option == "keyboard=none") {
      state.keyboardInteractivity = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
    } else if (option == "keyboard=on-demand") {
      state.keyboardInteractivity = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
    } else if (option == "keyboard=exclusive") {
      state.keyboardInteractivity = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
    } else if (option == "release-on-escape") {
      state.releaseOnEscape = true;
    } else if (option == "log-configures") {
      state.logConfigures = true;
    } else if (option == "popup-on-click") {
      state.popupOnClick = true;
    } else if (option == "popup-on-release") {
      state.popupOnRelease = true;
    } else {
      std::println(stderr, "layer-client: unknown option '{}'", option);
      return EXIT_FAILURE;
    }
  }

  const bool background = !bottom && !overlay && exclusiveHeight == 0;
  if (background) {
    state.fillColor = 0xFF5577AA;
  } else if (bottom) {
    state.fillColor = 0xFF00FF00;
  } else if (overlay) {
    state.fillColor = 0xFFFF0000;
  }
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "layer-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  wl_display_roundtrip(state.display);

  const auto selected =
      std::ranges::find_if(state.outputs, [&outputName](const auto& output) { return output->name == outputName; });
  if (state.compositor == nullptr
      || state.shm == nullptr
      || state.layerShell == nullptr
      || ((state.popupOnClick || state.popupOnRelease) && (state.wmBase == nullptr || state.seat == nullptr))
      || selected == state.outputs.end()) {
    std::println(stderr, "layer-client: missing protocol or output '{}'", outputName);
    return EXIT_FAILURE;
  }
  if (state.popupOnClick || state.popupOnRelease) {
    const uint32_t fillColor = state.fillColor;
    if (state.popupOnRelease) {
      state.fillColor = 0xFFFFAA00;
    }
    state.popupBuffer = createBuffer(state, kPopupSize, kPopupSize);
    state.fillColor = fillColor;
    if (state.popupBuffer.resource == nullptr) {
      std::println(stderr, "layer-client: failed to allocate the popup buffer");
      return EXIT_FAILURE;
    }
  }

  uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
  if (background) {
    layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
  } else if (bottom) {
    layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
  } else if (overlay) {
    layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
  }
  state.surface = wl_compositor_create_surface(state.compositor);
  state.layerSurface = zwlr_layer_shell_v1_get_layer_surface(
      state.layerShell, state.surface, (*selected)->resource, layer, "umbriel-output-restore-regression"
  );
  zwlr_layer_surface_v1_add_listener(state.layerSurface, &kLayerSurfaceListener, &state);
  if (state.keyboardInteractivity != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
    zwlr_layer_surface_v1_set_keyboard_interactivity(state.layerSurface, state.keyboardInteractivity);
  }
  uint32_t anchors = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
  if (bottom || overlay) {
    zwlr_layer_surface_v1_set_size(state.layerSurface, 200, 200);
    if (overlay) {
      zwlr_layer_surface_v1_set_exclusive_zone(state.layerSurface, -1);
    }
  } else {
    zwlr_layer_surface_v1_set_size(state.layerSurface, 0, background ? 0U : static_cast<uint32_t>(exclusiveHeight));
    anchors |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    if (background) {
      anchors |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
      zwlr_layer_surface_v1_set_exclusive_zone(state.layerSurface, -1);
    } else {
      zwlr_layer_surface_v1_set_exclusive_zone(state.layerSurface, exclusiveHeight);
    }
  }
  zwlr_layer_surface_v1_set_anchor(state.layerSurface, anchors);
  wl_surface_commit(state.surface);

  const bool control = std::getenv("LAYER_CONTROL") != nullptr;
  while (!state.closed && wl_display_dispatch_pending(state.display) >= 0) {
    if (wl_display_flush(state.display) < 0 && errno != EAGAIN) {
      state.failed = true;
      break;
    }
    pollfd sources[] = {
        {.fd = wl_display_get_fd(state.display), .events = POLLIN, .revents = 0},
        {.fd = control ? STDIN_FILENO : -1, .events = POLLIN, .revents = 0},
    };
    if (poll(sources, 2, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      state.failed = true;
      break;
    }
    if ((sources[0].revents & (POLLERR | POLLHUP)) != 0
        || ((sources[0].revents & POLLIN) != 0 && wl_display_dispatch(state.display) < 0)) {
      break;
    }
    if ((sources[1].revents & (POLLIN | POLLHUP)) != 0) {
      readCommands(state);
    }
  }

  if (state.frame != nullptr) {
    wl_callback_destroy(state.frame);
  }
  if (state.popup != nullptr) {
    xdg_popup_destroy(state.popup);
    xdg_surface_destroy(state.popupXdgSurface);
    wl_surface_destroy(state.popupSurface);
  }
  destroyBuffer(state.popupBuffer);
  if (state.childSubsurface != nullptr) {
    wl_subsurface_destroy(state.childSubsurface);
    wl_surface_destroy(state.childSurface);
  }
  destroyBuffer(state.childBuffer);
  if (state.pointer != nullptr) {
    wl_pointer_release(state.pointer);
  }
  if (state.wmBase != nullptr) {
    xdg_wm_base_destroy(state.wmBase);
  }
  zwlr_layer_surface_v1_destroy(state.layerSurface);
  wl_surface_destroy(state.surface);
  destroyBuffer(state.buffer);
  if (state.keyboard != nullptr) {
    wl_keyboard_release(state.keyboard);
  }
  if (state.seat != nullptr) {
    wl_seat_release(state.seat);
  }
  for (const auto& output : state.outputs) {
    wl_output_release(output->resource);
  }
  zwlr_layer_shell_v1_destroy(state.layerShell);
  wl_shm_destroy(state.shm);
  if (state.subcompositor != nullptr) {
    wl_subcompositor_destroy(state.subcompositor);
  }
  wl_compositor_destroy(state.compositor);
  wl_registry_destroy(registry);
  wl_display_disconnect(state.display);
  return state.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
