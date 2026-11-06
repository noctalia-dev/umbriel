// By default, maps a small layer surface and starts a Wayland data-device drag when its left mouse button is pressed.
// Target mode maps an xdg toplevel that accepts the offered text and reports enter and drop events. Tear-off mode maps
// an xdg source, then requests an input activation token with the saved press serial and maps another toplevel after
// the drag is cancelled over empty space. Toplevel mode attaches a real window
// and accepts stdin commands: o unmaps the origin, u unmaps the carried window,
// r remaps it, a attaches twice, d destroys the drag, b duplicates its source,
// s destroys the source, c uses it as a selection, g destroys the factory,
// t destroys the carried role, and q exits. The harness owns all input devices.

#include <wayland-client.h>

#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-toplevel-drag-v1-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

  constexpr int kSurfaceSize = 96;
  constexpr int kIconChildSize = 8;
  constexpr int kTargetWidth = 624;
  constexpr int kTargetHeight = 700;
  constexpr uint32_t kLeftButton = 0x110;

  struct DataOffer {
    wl_data_offer* resource = nullptr;
    bool hasText = false;
  };

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
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_data_device_manager* dataDeviceManager = nullptr;
    wl_data_device* dataDevice = nullptr;
    xdg_toplevel_drag_manager_v1* dragManager = nullptr;
    xdg_toplevel_drag_v1* toplevelDrag = nullptr;
    bool toplevelMode = false;
    bool existingToplevel = false;
    bool attachAfterMap = false;
    bool attachBeforeStart = false;
    xdg_activation_v1* activation = nullptr;
    xdg_activation_token_v1* activationRequest = nullptr;
    zwlr_layer_shell_v1* layerShell = nullptr;
    zwlr_layer_surface_v1* layerSurface = nullptr;
    xdg_wm_base* wmBase = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    xdg_surface* detachedXdgSurface = nullptr;
    xdg_toplevel* detachedToplevel = nullptr;
    wl_surface* surface = nullptr;
    wl_surface* detachedSurface = nullptr;
    wl_surface* iconSurface = nullptr;
    wl_surface* iconChildSurface = nullptr;
    wl_subsurface* iconChildSubsurface = nullptr;
    wl_data_source* source = nullptr;
    std::vector<std::unique_ptr<DataOffer>> offers;
    DataOffer* activeOffer = nullptr;
    Buffer windowBuffer;
    Buffer detachedBuffer;
    Buffer iconBuffer;
    Buffer iconChildBuffer;
    std::string activationToken;
    uint32_t dragSerial = 0;
    const char* title = "drag-target";
    bool targetMode = false;
    bool tearOffMode = false;
    bool ready = false;
    bool detachedMapped = false;
    bool dragStarted = false;
    bool dragFinished = false;
    bool waitForPointerRefresh = false;
    bool pointerRefreshObserved = false;
    bool complete = false;
    bool failed = false;
  };

  void mapDetached(State& state);
  void requestTearOffToken(State& state);

  Buffer createBuffer(State& state, int width, int height, uint32_t color) {
    Buffer buffer;
    const int stride = width * 4;
    buffer.size = static_cast<size_t>(stride * height);
    const int fd = memfd_create("umbriel-drag-client", MFD_CLOEXEC);
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

  void finishDrag(State& state, std::string_view result) {
    if (state.dragFinished) {
      return;
    }
    state.dragFinished = true;
    std::println("{}", result);
    std::fflush(stdout);

    if (state.toplevelMode) {
      return;
    }
    if (state.tearOffMode) {
      requestTearOffToken(state);
      mapDetached(state);
      return;
    }
    if (!state.waitForPointerRefresh) {
      state.complete = true;
    }
  }

  void sourceTarget(void*, wl_data_source*, const char*) {}

  void sourceSend(void*, wl_data_source*, const char*, int32_t fd) {
    constexpr std::string_view payload = "umbriel drag regression";
    size_t offset = 0;
    while (offset < payload.size()) {
      const ssize_t size = write(fd, payload.data() + offset, payload.size() - offset);
      if (size > 0) {
        offset += static_cast<size_t>(size);
        continue;
      }
      if (size < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    close(fd);
  }

  void sourceCancelled(void* data, wl_data_source*) { finishDrag(*static_cast<State*>(data), "drag-cancelled"); }

  void sourceDropPerformed(void*, wl_data_source*) {}

  void sourceFinished(void* data, wl_data_source*) { finishDrag(*static_cast<State*>(data), "drag-finished"); }

  void sourceAction(void*, wl_data_source*, uint32_t) {}

  constexpr wl_data_source_listener kDataSourceListener = {
      .target = sourceTarget,
      .send = sourceSend,
      .cancelled = sourceCancelled,
      .dnd_drop_performed = sourceDropPerformed,
      .dnd_finished = sourceFinished,
      .action = sourceAction,
  };

  void offerMime(void* data, wl_data_offer*, const char* mimeType) {
    auto& offer = *static_cast<DataOffer*>(data);
    offer.hasText = offer.hasText || std::string_view(mimeType) == "text/plain";
  }

  void offerSourceActions(void*, wl_data_offer*, uint32_t) {}
  void offerAction(void*, wl_data_offer*, uint32_t) {}

  constexpr wl_data_offer_listener kDataOfferListener = {
      .offer = offerMime,
      .source_actions = offerSourceActions,
      .action = offerAction,
  };

  DataOffer* findOffer(State& state, wl_data_offer* resource) {
    const auto found =
        std::ranges::find_if(state.offers, [resource](const auto& offer) { return offer->resource == resource; });
    return found != state.offers.end() ? found->get() : nullptr;
  }

  void deviceDataOffer(void* data, wl_data_device*, wl_data_offer* resource) {
    auto& state = *static_cast<State*>(data);
    auto offer = std::make_unique<DataOffer>();
    offer->resource = resource;
    wl_data_offer_add_listener(resource, &kDataOfferListener, offer.get());
    state.offers.push_back(std::move(offer));
  }

  void deviceEnter(
      void* data, wl_data_device*, uint32_t serial, wl_surface* surface, wl_fixed_t, wl_fixed_t, wl_data_offer* resource
  ) {
    auto& state = *static_cast<State*>(data);
    DataOffer* offer = findOffer(state, resource);
    if (!state.targetMode || surface != state.surface || offer == nullptr || !offer->hasText) {
      if (resource != nullptr) {
        wl_data_offer_accept(resource, serial, nullptr);
      }
      return;
    }
    state.activeOffer = offer;
    wl_data_offer_accept(resource, serial, "text/plain");
    wl_data_offer_set_actions(resource, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_display_flush(state.display);
    std::println("drag-enter");
    std::fflush(stdout);
  }

  void deviceLeave(void* data, wl_data_device*) {
    auto& state = *static_cast<State*>(data);
    if (state.activeOffer != nullptr) {
      std::println("drag-leave");
      std::fflush(stdout);
      state.activeOffer = nullptr;
    }
  }

  void deviceMotion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}

  void deviceDrop(void* data, wl_data_device*) {
    auto& state = *static_cast<State*>(data);
    if (state.activeOffer == nullptr) {
      return;
    }
    wl_data_offer_finish(state.activeOffer->resource);
    wl_display_flush(state.display);
    state.activeOffer = nullptr;
    state.complete = true;
    std::println("drop-received");
    std::fflush(stdout);
  }

  void deviceSelection(void*, wl_data_device*, wl_data_offer*) {}

  constexpr wl_data_device_listener kDataDeviceListener = {
      .data_offer = deviceDataOffer,
      .enter = deviceEnter,
      .leave = deviceLeave,
      .motion = deviceMotion,
      .drop = deviceDrop,
      .selection = deviceSelection,
  };

  void observePointerRefresh(State& state) {
    if (!state.waitForPointerRefresh || !state.dragFinished || state.pointerRefreshObserved) {
      return;
    }
    state.pointerRefreshObserved = true;
    state.complete = true;
    std::println("pointer-refreshed");
    std::fflush(stdout);
  }

  void pointerEnter(void* data, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t) {
    observePointerRefresh(*static_cast<State*>(data));
  }
  void pointerLeave(void*, wl_pointer*, uint32_t, wl_surface*) {}
  void pointerMotion(void* data, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {
    observePointerRefresh(*static_cast<State*>(data));
  }

  void activationTokenDone(void* data, xdg_activation_token_v1* request, const char* token) {
    auto& state = *static_cast<State*>(data);
    state.activationToken = token != nullptr ? token : "";
    xdg_activation_token_v1_destroy(request);
    state.activationRequest = nullptr;
    std::println("activation-token-ready");
    std::fflush(stdout);
    if (state.dragFinished) {
      mapDetached(state);
    }
  }

  constexpr xdg_activation_token_v1_listener kActivationTokenListener = {
      .done = activationTokenDone,
  };

  void requestTearOffToken(State& state) {
    if (!state.tearOffMode
        || state.dragSerial == 0
        || state.activationRequest != nullptr
        || !state.activationToken.empty()) {
      return;
    }
    state.activationRequest = xdg_activation_v1_get_activation_token(state.activation);
    xdg_activation_token_v1_add_listener(state.activationRequest, &kActivationTokenListener, &state);
    xdg_activation_token_v1_set_serial(state.activationRequest, state.dragSerial, state.seat);
    xdg_activation_token_v1_set_surface(state.activationRequest, state.surface);
    xdg_activation_token_v1_commit(state.activationRequest);
  }

  void pointerButton(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button, uint32_t buttonState) {
    auto& state = *static_cast<State*>(data);
    if (button != kLeftButton || buttonState != WL_POINTER_BUTTON_STATE_PRESSED || state.dragStarted) {
      return;
    }
    state.dragSerial = serial;

    state.source = wl_data_device_manager_create_data_source(state.dataDeviceManager);
    wl_data_source_add_listener(state.source, &kDataSourceListener, &state);
    wl_data_source_offer(state.source, "text/plain");
    wl_data_source_set_actions(state.source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    if (state.toplevelMode) {
      state.toplevelDrag = xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(state.dragManager, state.source);
      if (state.existingToplevel) {
        xdg_toplevel_drag_v1_attach(state.toplevelDrag, state.toplevel, 20, 30);
      } else if (state.attachBeforeStart) {
        mapDetached(state);
      }
    }

    state.iconSurface = wl_compositor_create_surface(state.compositor);
    wl_data_device_start_drag(state.dataDevice, state.source, state.surface, state.iconSurface, serial);

    // Create the child after start_drag so the compositor must also make a late drag-icon subsurface input-transparent.
    // The default surface input region covers this buffer.
    state.iconChildSurface = wl_compositor_create_surface(state.compositor);
    state.iconChildSubsurface =
        wl_subcompositor_get_subsurface(state.subcompositor, state.iconChildSurface, state.iconSurface);
    wl_subsurface_set_position(state.iconChildSubsurface, 0, 0);
    wl_surface_attach(state.iconChildSurface, state.iconChildBuffer.resource, 0, 0);
    wl_surface_damage_buffer(state.iconChildSurface, 0, 0, kIconChildSize, kIconChildSize);
    wl_surface_commit(state.iconChildSurface);

    wl_surface_attach(state.iconSurface, state.iconBuffer.resource, 0, 0);
    wl_surface_damage_buffer(state.iconSurface, 0, 0, 24, 24);
    wl_surface_commit(state.iconSurface);

    state.dragStarted = true;
    if (state.toplevelMode && !state.existingToplevel && !state.attachBeforeStart) {
      mapDetached(state);
    }
    std::println("drag-started");
    std::fflush(stdout);
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
    if (!state.targetMode && (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && state.pointer == nullptr) {
      state.pointer = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(state.pointer, &kPointerListener, &state);
    }
  }

  void seatName(void*, wl_seat*, const char*) {}

  constexpr wl_seat_listener kSeatListener = {
      .capabilities = seatCapabilities,
      .name = seatName,
  };

  void layerConfigure(void* data, zwlr_layer_surface_v1* layerSurface, uint32_t serial, uint32_t, uint32_t) {
    auto& state = *static_cast<State*>(data);
    zwlr_layer_surface_v1_ack_configure(layerSurface, serial);
    wl_surface_attach(state.surface, state.windowBuffer.resource, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, kSurfaceSize, kSurfaceSize);
    wl_surface_commit(state.surface);
    if (!state.ready) {
      state.ready = true;
      std::println("ready");
      std::fflush(stdout);
    }
  }

  void layerClosed(void* data, zwlr_layer_surface_v1*) {
    auto& state = *static_cast<State*>(data);
    state.failed = true;
    state.complete = true;
  }

  constexpr zwlr_layer_surface_v1_listener kLayerSurfaceListener = {
      .configure = layerConfigure,
      .closed = layerClosed,
  };

  void wmBasePing(void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }

  constexpr xdg_wm_base_listener kWmBaseListener = {
      .ping = wmBasePing,
  };

  void xdgSurfaceConfigure(void* data, xdg_surface* xdgSurface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    if (xdgSurface == state.detachedXdgSurface && state.detachedToplevel == nullptr) {
      // Destroying just the carried role leaves its xdg_surface alive. A
      // configure queued before destruction can still be dispatched by the
      // following roundtrip, but the server has already reset the role and
      // discarded its serials. Do not ack or commit that obsolete configure.
      return;
    }
    xdg_surface_ack_configure(xdgSurface, serial);
    if (xdgSurface == state.detachedXdgSurface) {
      wl_surface_attach(state.detachedSurface, state.detachedBuffer.resource, 0, 0);
      wl_surface_damage_buffer(state.detachedSurface, 0, 0, kTargetWidth, kTargetHeight);
      wl_surface_commit(state.detachedSurface);
      if (!state.detachedMapped) {
        state.detachedMapped = true;
        if (state.toplevelMode && state.attachAfterMap) {
          wl_display_roundtrip(state.display);
          xdg_toplevel_drag_v1_attach(state.toplevelDrag, state.detachedToplevel, 20, 30);
        }
        std::println("tear-off-mapped");
        std::fflush(stdout);
      }
      return;
    }
    if (!state.ready) {
      state.ready = true;
      wl_surface_attach(state.surface, state.windowBuffer.resource, 0, 0);
      wl_surface_damage_buffer(state.surface, 0, 0, kTargetWidth, kTargetHeight);
      std::println("ready");
      std::fflush(stdout);
    }
    wl_surface_commit(state.surface);
  }

  constexpr xdg_surface_listener kXdgSurfaceListener = {
      .configure = xdgSurfaceConfigure,
  };

  void toplevelConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}

  void toplevelClose(void* data, xdg_toplevel*) {
    auto& state = *static_cast<State*>(data);
    state.failed = true;
    state.complete = true;
  }

  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = nullptr,
      .wm_capabilities = nullptr,
  };

  void mapDetached(State& state) {
    if (state.detachedSurface != nullptr
        || (!state.toplevelMode && (!state.tearOffMode || !state.dragFinished || state.activationToken.empty()))) {
      return;
    }
    state.detachedSurface = wl_compositor_create_surface(state.compositor);
    state.detachedXdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.detachedSurface);
    xdg_surface_add_listener(state.detachedXdgSurface, &kXdgSurfaceListener, &state);
    state.detachedToplevel = xdg_surface_get_toplevel(state.detachedXdgSurface);
    xdg_toplevel_add_listener(state.detachedToplevel, &kToplevelListener, &state);
    xdg_toplevel_set_title(state.detachedToplevel, state.toplevelMode ? "toplevel-drag-window" : "tear-off-window");
    xdg_toplevel_set_app_id(state.detachedToplevel, "tear-off-window");
    if (state.toplevelMode) {
      if (!state.attachAfterMap) {
        xdg_toplevel_drag_v1_attach(state.toplevelDrag, state.detachedToplevel, 20, 30);
      }
    } else {
      xdg_activation_v1_activate(state.activation, state.activationToken.c_str(), state.detachedSurface);
    }
    wl_surface_commit(state.detachedSurface);
  }

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (!state.targetMode && std::strcmp(interface, wl_subcompositor_interface.name) == 0) {
      state.subcompositor =
          static_cast<wl_subcompositor*>(wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      state.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5U)));
      wl_seat_add_listener(state.seat, &kSeatListener, &state);
    } else if (std::strcmp(interface, wl_data_device_manager_interface.name) == 0) {
      state.dataDeviceManager = static_cast<wl_data_device_manager*>(
          wl_registry_bind(registry, name, &wl_data_device_manager_interface, std::min(version, 3U))
      );
    } else if (std::strcmp(interface, xdg_toplevel_drag_manager_v1_interface.name) == 0) {
      state.dragManager = static_cast<xdg_toplevel_drag_manager_v1*>(
          wl_registry_bind(registry, name, &xdg_toplevel_drag_manager_v1_interface, 1)
      );
    } else if (std::strcmp(interface, xdg_activation_v1_interface.name) == 0) {
      state.activation =
          static_cast<xdg_activation_v1*>(wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
      state.layerShell = static_cast<zwlr_layer_shell_v1*>(
          wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, std::min(version, 4U))
      );
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

  void destroyBuffer(Buffer& buffer) {
    if (buffer.resource != nullptr) {
      wl_buffer_destroy(buffer.resource);
    }
    if (buffer.pixels != MAP_FAILED) {
      munmap(buffer.pixels, buffer.size);
    }
  }

} // namespace

int main(int argc, char* argv[]) {
  const bool targetMode = argc >= 2 && std::string_view(argv[1]) == "target";
  const bool cursorRefresh = argc == 2 && std::string_view(argv[1]) == "cursor-refresh";
  const bool tearOffMode = argc == 2 && std::string_view(argv[1]) == "tear-off";
  const bool toplevelMode = argc >= 2 && std::string_view(argv[1]) == "toplevel";
  if ((!targetMode && argc > 1 && !cursorRefresh && !tearOffMode && !toplevelMode) || (targetMode && argc > 3)) {
    std::println(stderr, "usage: drag-client [cursor-refresh | tear-off | target [title] | toplevel [variant]]");
    return EXIT_FAILURE;
  }

  State state;
  state.toplevelMode = toplevelMode;
  state.existingToplevel = toplevelMode && argc == 3 && std::string_view(argv[2]) == "existing";
  state.attachAfterMap = toplevelMode && argc == 3 && std::string_view(argv[2]) == "after-map";
  state.attachBeforeStart = toplevelMode && argc == 3 && std::string_view(argv[2]) == "before-start";
  state.targetMode = targetMode;
  state.tearOffMode = tearOffMode;
  state.title = targetMode && argc == 3 ? argv[2] : (tearOffMode ? "tear-off-source" : "drag-target");
  if (toplevelMode) {
    state.title = "toplevel-drag-source";
  }
  state.waitForPointerRefresh = cursorRefresh;
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "drag-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }

  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  wl_display_roundtrip(state.display);

  const bool targetGlobals = state.targetMode && state.wmBase != nullptr;
  const bool sourceGlobals = !state.targetMode
      && state.pointer != nullptr
      && state.subcompositor != nullptr
      && (state.toplevelMode ? state.wmBase != nullptr && state.dragManager != nullptr
                             : (state.tearOffMode ? state.wmBase != nullptr && state.activation != nullptr
                                                  : state.layerShell != nullptr));
  if (state.compositor == nullptr
      || state.shm == nullptr
      || state.seat == nullptr
      || state.dataDeviceManager == nullptr
      || (!targetGlobals && !sourceGlobals)) {
    std::println(stderr, "drag-client: compositor is missing a required Wayland global");
    return EXIT_FAILURE;
  }

  state.dataDevice = wl_data_device_manager_get_data_device(state.dataDeviceManager, state.seat);
  wl_data_device_add_listener(state.dataDevice, &kDataDeviceListener, &state);
  const bool xdgSource = state.targetMode || state.tearOffMode || state.toplevelMode;
  state.windowBuffer = createBuffer(
      state, xdgSource ? kTargetWidth : kSurfaceSize, xdgSource ? kTargetHeight : kSurfaceSize,
      state.targetMode ? 0xFF55AA77 : 0xFF4477CC
  );
  if (state.tearOffMode || state.toplevelMode) {
    state.detachedBuffer = createBuffer(state, kTargetWidth, kTargetHeight, 0xFFAA5577);
  }
  if (!state.targetMode) {
    state.iconBuffer = createBuffer(state, 24, 24, 0xFFFFAA22);
    state.iconChildBuffer = createBuffer(state, kIconChildSize, kIconChildSize, 0xFFCC44FF);
  }
  if (state.windowBuffer.resource == nullptr
      || ((state.tearOffMode || state.toplevelMode) && state.detachedBuffer.resource == nullptr)
      || (!state.targetMode && (state.iconBuffer.resource == nullptr || state.iconChildBuffer.resource == nullptr))) {
    std::println(stderr, "drag-client: failed to allocate shared-memory buffers");
    return EXIT_FAILURE;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  if (xdgSource) {
    state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
    xdg_surface_add_listener(state.xdgSurface, &kXdgSurfaceListener, &state);
    state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
    xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
    xdg_toplevel_set_title(state.toplevel, state.title);
    xdg_toplevel_set_app_id(state.toplevel, state.title);
  } else {
    state.layerSurface = zwlr_layer_shell_v1_get_layer_surface(
        state.layerShell, state.surface, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "umbriel-drag-regression"
    );
    zwlr_layer_surface_v1_add_listener(state.layerSurface, &kLayerSurfaceListener, &state);
    zwlr_layer_surface_v1_set_size(state.layerSurface, kSurfaceSize, kSurfaceSize);
    zwlr_layer_surface_v1_set_anchor(
        state.layerSurface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
    );
    zwlr_layer_surface_v1_set_margin(state.layerSurface, 16, 0, 0, 16);
    zwlr_layer_surface_v1_set_exclusive_zone(state.layerSurface, 0);
  }
  wl_surface_commit(state.surface);

  while (!state.complete) {
    if (!state.toplevelMode) {
      if (wl_display_dispatch(state.display) < 0) {
        break;
      }
      continue;
    }
    if (wl_display_dispatch_pending(state.display) < 0 || wl_display_flush(state.display) < 0) {
      break;
    }
    pollfd fds[] = {{wl_display_get_fd(state.display), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    if (poll(fds, 2, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0 && wl_display_dispatch(state.display) < 0) {
      break;
    }
    if ((fds[1].revents & POLLHUP) != 0) {
      break;
    }
    if ((fds[1].revents & POLLIN) == 0) {
      continue;
    }
    char command = 0;
    if (read(STDIN_FILENO, &command, 1) != 1) {
      break;
    }
    if (command == '\n') {
      continue;
    }
    switch (command) {
    case 'o':
      wl_surface_attach(state.surface, nullptr, 0, 0);
      wl_surface_commit(state.surface);
      break;
    case 'u':
      wl_surface_attach(state.detachedSurface, nullptr, 0, 0);
      wl_surface_commit(state.detachedSurface);
      wl_display_roundtrip(state.display);
      state.detachedMapped = false;
      break;
    case 'r':
      xdg_toplevel_set_title(state.detachedToplevel, "toplevel-drag-window");
      xdg_toplevel_set_app_id(state.detachedToplevel, "toplevel-drag-window");
      xdg_toplevel_drag_v1_attach(state.toplevelDrag, state.detachedToplevel, 20, 30);
      wl_surface_commit(state.detachedSurface);
      break;
    case 'a':
      xdg_toplevel_drag_v1_attach(
          state.toplevelDrag, state.existingToplevel ? state.toplevel : state.detachedToplevel, 20, 30
      );
      break;
    case 'd':
      xdg_toplevel_drag_v1_destroy(state.toplevelDrag);
      state.toplevelDrag = nullptr;
      break;
    case 'b':
      xdg_toplevel_drag_manager_v1_get_xdg_toplevel_drag(state.dragManager, state.source);
      break;
    case 's':
      wl_data_source_destroy(state.source);
      state.source = nullptr;
      break;
    case 'c':
      wl_data_device_set_selection(state.dataDevice, state.source, state.dragSerial);
      break;
    case 'g':
      xdg_toplevel_drag_manager_v1_destroy(state.dragManager);
      state.dragManager = nullptr;
      break;
    case 't':
      xdg_toplevel_destroy(state.detachedToplevel);
      state.detachedToplevel = nullptr;
      break;
    case 'q':
      state.complete = true;
      break;
    default:
      state.failed = true;
      state.complete = true;
      break;
    }
    if (wl_display_roundtrip(state.display) < 0) {
      break;
    }
    std::println("command {}", command);
    std::fflush(stdout);
  }
  if (wl_display_get_error(state.display) != 0) {
    const wl_interface* interface = nullptr;
    uint32_t id = 0;
    const uint32_t code = wl_display_get_protocol_error(state.display, &interface, &id);
    std::println("protocol-error {} {}", interface != nullptr ? interface->name : "none", code);
    std::fflush(stdout);
    state.failed = true;
  }
  wl_display_roundtrip(state.display);

  if (state.toplevelDrag != nullptr) {
    xdg_toplevel_drag_v1_destroy(state.toplevelDrag);
  }
  if (state.source != nullptr) {
    wl_data_source_destroy(state.source);
  }
  if (state.activationRequest != nullptr) {
    xdg_activation_token_v1_destroy(state.activationRequest);
  }
  if (state.iconChildSubsurface != nullptr) {
    wl_subsurface_destroy(state.iconChildSubsurface);
  }
  if (state.iconChildSurface != nullptr) {
    wl_surface_destroy(state.iconChildSurface);
  }
  if (state.iconSurface != nullptr) {
    wl_surface_destroy(state.iconSurface);
  }
  for (const auto& offer : state.offers) {
    wl_data_offer_destroy(offer->resource);
  }
  if (state.layerSurface != nullptr) {
    zwlr_layer_surface_v1_destroy(state.layerSurface);
  }
  if (state.detachedToplevel != nullptr) {
    xdg_toplevel_destroy(state.detachedToplevel);
  }
  if (state.detachedXdgSurface != nullptr) {
    xdg_surface_destroy(state.detachedXdgSurface);
  }
  if (state.detachedSurface != nullptr) {
    wl_surface_destroy(state.detachedSurface);
  }
  if (state.toplevel != nullptr) {
    xdg_toplevel_destroy(state.toplevel);
  }
  if (state.xdgSurface != nullptr) {
    xdg_surface_destroy(state.xdgSurface);
  }
  wl_surface_destroy(state.surface);
  wl_data_device_release(state.dataDevice);
  if (state.pointer != nullptr) {
    wl_pointer_release(state.pointer);
  }
  destroyBuffer(state.iconChildBuffer);
  destroyBuffer(state.iconBuffer);
  destroyBuffer(state.detachedBuffer);
  destroyBuffer(state.windowBuffer);
  if (state.activation != nullptr) {
    xdg_activation_v1_destroy(state.activation);
  }
  wl_display_disconnect(state.display);
  return state.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
