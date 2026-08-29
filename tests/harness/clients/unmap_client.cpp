// Maps an xdg toplevel, then on a compositor close request unmaps it without destroying the surface. The compositor
// therefore keeps the View registered and unmapped, which is what makes this client useful: closing a window this way
// never reaches Server::removeView, so any focus reassignment the compositor performs must happen at unmap time, the
// same point a card disappears from the overview. Prints "mapped" once the toplevel is up and "unmapped" once the close
// request lands, then keeps the connection alive until the harness kills it. Usage: unmap-client [title [width
// height]]. The optional dimensions let pointer checks expose a surface that fills its assigned tile. With
// REMAP_ON_STDIN set, reading any byte performs a fresh initial commit and maps the same toplevel again.
// With ACTIVATION_TOKEN_FILE set, an `a` command reads and uses that token. A `c` command requests an ordinary client
// token instead. When the surface is hidden, either command queues activation before the remap commit.
// CONTENT_TYPE sets a surface hint before its initial commit. CONTENT_TYPE_ON_SUBSURFACE places it on a rendering
// child, matching current Proton behavior. XDG_TAG sets a toplevel tag before the initial commit.
// CONTENT_TYPE_AFTER_MAP, XDG_TAG_AFTER_MAP, and TITLE_AFTER_MAP update their metadata on stdin.

#include "color-management-v1-client-protocol.h"
#include "content-type-v1-client-protocol.h"
#include "tearing-control-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-toplevel-tag-v1-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <print>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {

#ifdef WP_COLOR_MANAGER_V1_CREATE_WINDOWS_BT2100_SINCE_VERSION
  constexpr uint32_t kColorManagerVersion = WP_COLOR_MANAGER_V1_CREATE_WINDOWS_BT2100_SINCE_VERSION;
#else
  constexpr uint32_t kColorManagerVersion = 2;
#endif

  int parseContentType(const char* value) {
    if (value == nullptr || std::strcmp(value, "none") == 0) {
      return WP_CONTENT_TYPE_V1_TYPE_NONE;
    }
    if (std::strcmp(value, "photo") == 0) {
      return WP_CONTENT_TYPE_V1_TYPE_PHOTO;
    }
    if (std::strcmp(value, "video") == 0) {
      return WP_CONTENT_TYPE_V1_TYPE_VIDEO;
    }
    if (std::strcmp(value, "game") == 0) {
      return WP_CONTENT_TYPE_V1_TYPE_GAME;
    }
    return -1;
  }

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
    xdg_activation_v1* activation = nullptr;
    xdg_toplevel_tag_manager_v1* xdgTagManager = nullptr;
    wp_content_type_manager_v1* contentTypeManager = nullptr;
    wp_content_type_v1* contentType = nullptr;
    wl_surface* contentTypeSurface = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    wl_seat* seat = nullptr;
    wl_keyboard* keyboard = nullptr;
    wp_color_manager_v1* colorManager = nullptr;
    wp_color_management_surface_v1* colorSurface = nullptr;
    wp_image_description_v1* imageDescription = nullptr;
    wp_tearing_control_manager_v1* tearingManager = nullptr;
    wp_tearing_control_v1* tearingControl = nullptr;
    Buffer buffer;
    wl_surface* colorChildSurface = nullptr;
    wl_subsurface* colorChildSubsurface = nullptr;
    Buffer colorChildBuffer;
    int width = 64;
    int height = 64;
    bool mapped = false;
    bool closed = false;
    bool redrawOnClose = false;
    bool redrawOnceOnClose = false;
    bool keyboardFocused = false;
    bool requestMaximized = false;
    bool requestMaximizedAfterConfigure = false;
    bool maximizeRequested = false;
    bool logConfigures = false;
    bool requestFullscreen = false;
    bool fullscreenRequested = false;
    bool requestHdr = false;
    bool requestWindowsScrgb = false;
    bool requestWindowsBt2100 = false;
    bool colorOnSubsurface = false;
    bool contentTypeOnSubsurface = false;
    bool colorChildLifecycle = false;
    uint32_t colorChildLifecyclePhase = 0;
    bool hasExtendedTargetVolume = false;
    bool hasWindowsScrgb = false;
#ifdef WP_COLOR_MANAGER_V1_CREATE_WINDOWS_BT2100_SINCE_VERSION
    bool hasWindowsBt2100 = false;
#endif
    bool colorManagerDone = false;
    bool imageDescriptionReady = false;
    bool imageDescriptionFailed = false;
    bool metadataUpdated = false;
    int tearingHint = -1;
    const char* title = "unmap-client";
    const char* appId = nullptr;
    const char* remapAppId = nullptr;
  };

  struct AuxiliaryToplevel {
    State* state = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    Buffer buffer;
    bool mapped = false;
  };

  void colorManagerSupportedIntent(void*, wp_color_manager_v1*, uint32_t) {}

  void colorManagerSupportedFeature(void* data, wp_color_manager_v1*, uint32_t feature) {
    auto& state = *static_cast<State*>(data);
    if (feature == WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME) {
      state.hasExtendedTargetVolume = true;
    } else if (feature == WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB) {
      state.hasWindowsScrgb = true;
#ifdef WP_COLOR_MANAGER_V1_CREATE_WINDOWS_BT2100_SINCE_VERSION
    } else if (feature == WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_BT2100) {
      state.hasWindowsBt2100 = true;
#endif
    }
  }

  void colorManagerSupportedTransferFunction(void*, wp_color_manager_v1*, uint32_t) {}
  void colorManagerSupportedPrimaries(void*, wp_color_manager_v1*, uint32_t) {}

  void colorManagerDone(void* data, wp_color_manager_v1*) { static_cast<State*>(data)->colorManagerDone = true; }

  constexpr wp_color_manager_v1_listener kColorManagerListener = {
      .supported_intent = colorManagerSupportedIntent,
      .supported_feature = colorManagerSupportedFeature,
      .supported_tf_named = colorManagerSupportedTransferFunction,
      .supported_primaries_named = colorManagerSupportedPrimaries,
      .done = colorManagerDone,
  };

  void imageDescriptionFailed(void* data, wp_image_description_v1*, uint32_t, const char* message) {
    auto& state = *static_cast<State*>(data);
    state.imageDescriptionFailed = true;
    std::println(stderr, "unmap-client: HDR image description failed: {}", message);
  }

  void imageDescriptionReady(void* data, wp_image_description_v1*, uint32_t) {
    static_cast<State*>(data)->imageDescriptionReady = true;
  }

  void imageDescriptionReady2(void* data, wp_image_description_v1*, uint32_t, uint32_t) {
    static_cast<State*>(data)->imageDescriptionReady = true;
  }

  constexpr wp_image_description_v1_listener kImageDescriptionListener = {
      .failed = imageDescriptionFailed,
      .ready = imageDescriptionReady,
      .ready2 = imageDescriptionReady2,
  };

  void keyboardKeymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); }

  void keyboardEnter(void* data, wl_keyboard*, uint32_t, wl_surface* surface, wl_array*) {
    auto& state = *static_cast<State*>(data);
    state.keyboardFocused = surface == state.surface;
    if (state.keyboardFocused) {
      std::println("keyboard-enter");
      std::fflush(stdout);
    }
  }

  void keyboardLeave(void* data, wl_keyboard*, uint32_t, wl_surface* surface) {
    auto& state = *static_cast<State*>(data);
    if (surface == state.surface) {
      state.keyboardFocused = false;
      std::println("keyboard-leave");
      std::fflush(stdout);
    }
  }

  void keyboardKey(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t keyState) {
    auto& state = *static_cast<State*>(data);
    if (state.keyboardFocused) {
      std::println("key {} {}", key, keyState);
      std::fflush(stdout);
    }
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

  void seatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) {
    auto& state = *static_cast<State*>(data);
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && state.keyboard == nullptr) {
      state.keyboard = wl_seat_get_keyboard(seat);
      wl_keyboard_add_listener(state.keyboard, &kKeyboardListener, &state);
    }
  }

  void seatName(void*, wl_seat*, const char*) {}

  constexpr wl_seat_listener kSeatListener = {
      .capabilities = seatCapabilities,
      .name = seatName,
  };

  Buffer createBuffer(State& state) {
    Buffer buffer;
    const int stride = state.width * 4;
    buffer.size = static_cast<size_t>(stride * state.height);
    const int fd = memfd_create("umbriel-unmap-client", MFD_CLOEXEC);
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
    std::fill_n(static_cast<uint32_t*>(buffer.pixels), buffer.size / sizeof(uint32_t), 0xFF5577AA);

    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(buffer.size));
    buffer.resource = wl_shm_pool_create_buffer(pool, 0, state.width, state.height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
  }

  void auxiliaryXdgSurfaceConfigure(void* data, xdg_surface* xdgSurface, uint32_t serial) {
    auto& window = *static_cast<AuxiliaryToplevel*>(data);
    xdg_surface_ack_configure(xdgSurface, serial);
    if (window.mapped) {
      return;
    }
    window.mapped = true;
    wl_surface_attach(window.surface, window.buffer.resource, 0, 0);
    wl_surface_damage_buffer(window.surface, 0, 0, window.state->width, window.state->height);
    wl_surface_commit(window.surface);
  }

  constexpr xdg_surface_listener kAuxiliaryXdgSurfaceListener = {
      .configure = auxiliaryXdgSurfaceConfigure,
  };

  void auxiliaryToplevelConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}

  void auxiliaryToplevelClose(void* data, xdg_toplevel*) {
    auto& window = *static_cast<AuxiliaryToplevel*>(data);
    if (!window.mapped) {
      return;
    }
    window.mapped = false;
    wl_surface_attach(window.surface, nullptr, 0, 0);
    wl_surface_commit(window.surface);
  }

  constexpr xdg_toplevel_listener kAuxiliaryToplevelListener = {
      .configure = auxiliaryToplevelConfigure,
      .close = auxiliaryToplevelClose,
      .configure_bounds = nullptr,
      .wm_capabilities = nullptr,
  };

  bool mapAuxiliaryToplevel(State& state, AuxiliaryToplevel& window, const char* title) {
    window.state = &state;
    window.buffer = createBuffer(state);
    if (window.buffer.resource == nullptr) {
      return false;
    }
    window.surface = wl_compositor_create_surface(state.compositor);
    window.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, window.surface);
    xdg_surface_add_listener(window.xdgSurface, &kAuxiliaryXdgSurfaceListener, &window);
    window.toplevel = xdg_surface_get_toplevel(window.xdgSurface);
    xdg_toplevel_add_listener(window.toplevel, &kAuxiliaryToplevelListener, &window);
    xdg_toplevel_set_title(window.toplevel, title);
    wl_surface_commit(window.surface);
    while (!window.mapped) {
      if (wl_display_dispatch(state.display) < 0) {
        return false;
      }
    }
    return true;
  }

  void xdgSurfaceConfigure(void* data, xdg_surface* xdgSurface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(xdgSurface, serial);
    if (state.mapped) {
      // Apply later toplevel state transitions, such as leaving fullscreen. Acknowledging the configure without a
      // surface commit leaves the requested state pending forever.
      wl_surface_commit(state.surface);
      return;
    }
    if (state.closed) {
      return;
    }
    if (state.requestMaximizedAfterConfigure && !state.maximizeRequested) {
      xdg_toplevel_set_maximized(state.toplevel);
      wl_surface_commit(state.surface);
      state.maximizeRequested = true;
      return;
    }
    state.mapped = true;
    wl_surface_attach(state.surface, state.buffer.resource, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, state.width, state.height);
    wl_surface_commit(state.surface);
    if (state.requestFullscreen && !state.fullscreenRequested) {
      xdg_toplevel_set_fullscreen(state.toplevel, nullptr);
      wl_surface_commit(state.surface);
      state.fullscreenRequested = true;
    }
    std::println("mapped");
    std::fflush(stdout);
  }

  constexpr xdg_surface_listener kXdgSurfaceListener = {
      .configure = xdgSurfaceConfigure,
  };

  void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array* states) {
    auto& state = *static_cast<State*>(data);
    if (state.logConfigures) {
      std::println("configured-size={}x{}", width, height);
      std::fflush(stdout);
    }
    const auto* configured = static_cast<const uint32_t*>(states->data);
    const size_t count = states->size / sizeof(uint32_t);
    for (size_t index = 0; index < count; ++index) {
      if (configured[index] == XDG_TOPLEVEL_STATE_MAXIMIZED) {
        std::println("configured-maximized");
        std::fflush(stdout);
        break;
      }
    }
  }

  void toplevelClose(void* data, xdg_toplevel*) {
    auto& state = *static_cast<State*>(data);
    if (!state.mapped) {
      return;
    }
    if (state.redrawOnClose) {
      wl_surface_damage_buffer(state.surface, 0, 0, state.width, state.height);
      wl_surface_commit(state.surface);
      if (state.redrawOnceOnClose) {
        state.redrawOnClose = false;
      }
      std::println("redrawn");
      std::fflush(stdout);
      return;
    }
    if (state.colorChildLifecycle) {
      if (state.colorChildLifecyclePhase == 0) {
        wl_surface_attach(state.colorChildSurface, state.colorChildBuffer.resource, 0, 0);
        wl_surface_damage_buffer(state.colorChildSurface, 0, 0, state.width, state.height);
        wl_surface_commit(state.colorChildSurface);
        ++state.colorChildLifecyclePhase;
        std::println("color-child-mapped");
        std::fflush(stdout);
        return;
      }
      if (state.colorChildLifecyclePhase == 1) {
        wp_color_management_surface_v1_set_image_description(
            state.colorSurface, state.imageDescription, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL
        );
        wl_surface_commit(state.colorChildSurface);
        ++state.colorChildLifecyclePhase;
        std::println("color-child-hdr");
        std::fflush(stdout);
        return;
      }
      if (state.colorChildLifecyclePhase == 2) {
        wl_surface_attach(state.colorChildSurface, nullptr, 0, 0);
        wl_surface_commit(state.colorChildSurface);
        ++state.colorChildLifecyclePhase;
        std::println("color-child-unmapped");
        std::fflush(stdout);
        return;
      }
    }
    state.mapped = false;
    state.closed = true;
    // Attaching a null buffer unmaps the surface; the toplevel stays alive.
    wl_surface_attach(state.surface, nullptr, 0, 0);
    wl_surface_commit(state.surface);
    std::println("unmapped");
    std::fflush(stdout);
  }

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
    } else if (std::strcmp(interface, xdg_activation_v1_interface.name) == 0) {
      state.activation = static_cast<xdg_activation_v1*>(
          wl_registry_bind(registry, name, &xdg_activation_v1_interface, std::min(version, 1U))
      );
    } else if (std::strcmp(interface, xdg_toplevel_tag_manager_v1_interface.name) == 0) {
      state.xdgTagManager = static_cast<xdg_toplevel_tag_manager_v1*>(
          wl_registry_bind(registry, name, &xdg_toplevel_tag_manager_v1_interface, std::min(version, 1U))
      );
    } else if (std::strcmp(interface, wp_content_type_manager_v1_interface.name) == 0) {
      state.contentTypeManager = static_cast<wp_content_type_manager_v1*>(
          wl_registry_bind(registry, name, &wp_content_type_manager_v1_interface, std::min(version, 1U))
      );
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      state.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 2U)));
      wl_seat_add_listener(state.seat, &kSeatListener, &state);
    } else if (std::strcmp(interface, wp_color_manager_v1_interface.name) == 0) {
      state.colorManager = static_cast<wp_color_manager_v1*>(
          wl_registry_bind(registry, name, &wp_color_manager_v1_interface, std::min(version, kColorManagerVersion))
      );
      wp_color_manager_v1_add_listener(state.colorManager, &kColorManagerListener, &state);
    } else if (std::strcmp(interface, wp_tearing_control_manager_v1_interface.name) == 0) {
      state.tearingManager = static_cast<wp_tearing_control_manager_v1*>(
          wl_registry_bind(registry, name, &wp_tearing_control_manager_v1_interface, std::min(version, 1U))
      );
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

  bool activateFromFile(State& state, const char* path) {
    if (path == nullptr) {
      std::println(stderr, "unmap-client: activation command needs ACTIVATION_TOKEN_FILE");
      return false;
    }
    FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
      std::println(stderr, "unmap-client: cannot open activation token file");
      return false;
    }
    char token[256]{};
    const bool readToken = std::fgets(token, sizeof(token), file) != nullptr;
    std::fclose(file);
    token[std::strcspn(token, "\r\n")] = '\0';
    if (!readToken || token[0] == '\0') {
      std::println(stderr, "unmap-client: activation token file is empty");
      return false;
    }
    xdg_activation_v1_activate(state.activation, token, state.surface);
    std::println("activation-sent");
    std::fflush(stdout);
    return true;
  }

  void queueRemap(State& state) {
    state.closed = false;
    xdg_toplevel_set_title(state.toplevel, state.title);
    if (state.remapAppId != nullptr) {
      xdg_toplevel_set_app_id(state.toplevel, state.remapAppId);
    }
    wl_surface_attach(state.surface, nullptr, 0, 0);
    wl_surface_commit(state.surface);
    std::println("remap-requested");
    std::fflush(stdout);
  }

  void clientActivationTokenDone(void* data, xdg_activation_token_v1* activationToken, const char* token) {
    auto& state = *static_cast<State*>(data);
    xdg_activation_v1_activate(state.activation, token, state.surface);
    xdg_activation_token_v1_destroy(activationToken);
    std::println("activation-sent");
    std::fflush(stdout);
    if (!state.mapped) {
      queueRemap(state);
    }
  }

  constexpr xdg_activation_token_v1_listener kActivationTokenListener = {
      .done = clientActivationTokenDone,
  };

  bool activateWithClientToken(State& state) {
    if (state.activation == nullptr) {
      std::println(stderr, "unmap-client: compositor is missing xdg_activation_v1");
      return false;
    }
    xdg_activation_token_v1* token = xdg_activation_v1_get_activation_token(state.activation);
    if (token == nullptr) {
      std::println(stderr, "unmap-client: failed to create activation token");
      return false;
    }
    xdg_activation_token_v1_add_listener(token, &kActivationTokenListener, &state);
    xdg_activation_token_v1_set_surface(token, state.surface);
    xdg_activation_token_v1_commit(token);
    std::println("activation-requested");
    std::fflush(stdout);
    return true;
  }

  void destroyAuxiliaryToplevel(AuxiliaryToplevel& window) {
    if (window.toplevel != nullptr) {
      xdg_toplevel_destroy(window.toplevel);
    }
    if (window.xdgSurface != nullptr) {
      xdg_surface_destroy(window.xdgSurface);
    }
    if (window.surface != nullptr) {
      wl_surface_destroy(window.surface);
    }
    destroyBuffer(window.buffer);
  }

} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--color-manager-version") == 0) {
    std::println("{}", kColorManagerVersion);
    return EXIT_SUCCESS;
  }

  State state;
  state.title = argc > 1 ? argv[1] : "unmap-client";
  state.appId = std::getenv("APP_ID");
  state.remapAppId = std::getenv("APP_ID_AFTER_ACTIVATION");
  if (state.remapAppId == nullptr) {
    state.remapAppId = state.appId;
  }
  const bool remapOnStdin = std::getenv("REMAP_ON_STDIN") != nullptr;
  const char* activationTokenFile = std::getenv("ACTIVATION_TOKEN_FILE");
  const char* initialContentType = std::getenv("CONTENT_TYPE");
  const char* updatedContentType = std::getenv("CONTENT_TYPE_AFTER_MAP");
  const char* initialXdgTag = std::getenv("XDG_TAG");
  const char* updatedXdgTag = std::getenv("XDG_TAG_AFTER_MAP");
  const char* updatedTitle = std::getenv("TITLE_AFTER_MAP");
  const bool updateOnStdin = updatedContentType != nullptr || updatedXdgTag != nullptr || updatedTitle != nullptr;
  if (parseContentType(initialContentType) < 0 || parseContentType(updatedContentType) < 0) {
    std::println(stderr, "unmap-client: CONTENT_TYPE values must be none, photo, video, or game");
    return EXIT_FAILURE;
  }
  if (const char* redraw = std::getenv("REDRAW_ON_CLOSE")) {
    state.redrawOnClose = true;
    state.redrawOnceOnClose = std::string_view(redraw) == "once";
  }
  state.requestMaximized = std::getenv("REQUEST_MAXIMIZED") != nullptr;
  state.requestMaximizedAfterConfigure = std::getenv("REQUEST_MAXIMIZED_AFTER_CONFIGURE") != nullptr;
  state.logConfigures = std::getenv("LOG_CONFIGURES") != nullptr;
  state.requestFullscreen = std::getenv("REQUEST_FULLSCREEN") != nullptr;
  state.requestHdr = std::getenv("COLOR_HDR") != nullptr;
  state.requestWindowsScrgb = std::getenv("COLOR_WINDOWS_SCRGB") != nullptr;
  state.requestWindowsBt2100 = std::getenv("COLOR_WINDOWS_BT2100") != nullptr;
  state.colorOnSubsurface = std::getenv("COLOR_ON_SUBSURFACE") != nullptr;
  state.contentTypeOnSubsurface = std::getenv("CONTENT_TYPE_ON_SUBSURFACE") != nullptr;
  state.colorChildLifecycle = std::getenv("COLOR_CHILD_LIFECYCLE") != nullptr;
  if (const char* hint = std::getenv("TEARING_HINT")) {
    if (std::strcmp(hint, "async") == 0) {
      state.tearingHint = WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
    } else if (std::strcmp(hint, "vsync") == 0) {
      state.tearingHint = WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC;
    } else {
      std::println(stderr, "unmap-client: TEARING_HINT must be async or vsync");
      return EXIT_FAILURE;
    }
  }
  if (argc > 2) {
    state.width = std::max(1, std::atoi(argv[2]));
  }
  if (argc > 3) {
    state.height = std::max(1, std::atoi(argv[3]));
  }
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "unmap-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }

  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  wl_display_roundtrip(state.display);

  if (state.compositor == nullptr || state.shm == nullptr || state.wmBase == nullptr) {
    std::println(stderr, "unmap-client: compositor is missing a required Wayland global");
    return EXIT_FAILURE;
  }
  if (activationTokenFile != nullptr && state.activation == nullptr) {
    std::println(stderr, "unmap-client: compositor is missing xdg_activation_v1");
    return EXIT_FAILURE;
  }
  if ((initialContentType != nullptr || updatedContentType != nullptr) && state.contentTypeManager == nullptr) {
    std::println(stderr, "unmap-client: compositor is missing wp_content_type_manager_v1");
    return EXIT_FAILURE;
  }
  if ((initialXdgTag != nullptr || updatedXdgTag != nullptr) && state.xdgTagManager == nullptr) {
    std::println(stderr, "unmap-client: compositor is missing xdg_toplevel_tag_manager_v1");
    return EXIT_FAILURE;
  }

  state.buffer = createBuffer(state);
  if (state.buffer.resource == nullptr) {
    std::println(stderr, "unmap-client: failed to allocate shared-memory buffer");
    return EXIT_FAILURE;
  }

  const bool transientSuite = std::getenv("TRANSIENT_SUITE") != nullptr;
  AuxiliaryToplevel transientParent;
  AuxiliaryToplevel transientUnrelated;
  if (transientSuite
      && (!mapAuxiliaryToplevel(state, transientParent, "transient-parent")
          || !mapAuxiliaryToplevel(state, transientUnrelated, "transient-unrelated"))) {
    std::println(stderr, "unmap-client: failed to map transient-suite support windows");
    return EXIT_FAILURE;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  if (state.tearingHint >= 0) {
    if (state.tearingManager == nullptr) {
      std::println(stderr, "unmap-client: compositor is missing wp_tearing_control_manager_v1");
      return EXIT_FAILURE;
    }
    state.tearingControl = wp_tearing_control_manager_v1_get_tearing_control(state.tearingManager, state.surface);
    wp_tearing_control_v1_set_presentation_hint(state.tearingControl, static_cast<uint32_t>(state.tearingHint));
  }
  wl_surface* colorTargetSurface = state.surface;
  if (state.colorOnSubsurface || state.contentTypeOnSubsurface) {
    if (state.subcompositor == nullptr) {
      std::println(stderr, "unmap-client: compositor is missing wl_subcompositor");
      return EXIT_FAILURE;
    }
    state.colorChildBuffer = createBuffer(state);
    if (state.colorChildBuffer.resource == nullptr) {
      std::println(stderr, "unmap-client: failed to allocate color child buffer");
      return EXIT_FAILURE;
    }
    state.colorChildSurface = wl_compositor_create_surface(state.compositor);
    state.colorChildSubsurface =
        wl_subcompositor_get_subsurface(state.subcompositor, state.colorChildSurface, state.surface);
    wl_subsurface_set_desync(state.colorChildSubsurface);
    if (state.colorOnSubsurface) {
      colorTargetSurface = state.colorChildSurface;
    }
  }
  if (initialContentType != nullptr || updatedContentType != nullptr) {
    state.contentTypeSurface = state.contentTypeOnSubsurface ? state.colorChildSurface : state.surface;
    state.contentType =
        wp_content_type_manager_v1_get_surface_content_type(state.contentTypeManager, state.contentTypeSurface);
    if (initialContentType != nullptr) {
      wp_content_type_v1_set_content_type(
          state.contentType, static_cast<uint32_t>(parseContentType(initialContentType))
      );
    }
  }
  if (state.requestHdr || state.requestWindowsScrgb || state.requestWindowsBt2100) {
    if (state.colorManager == nullptr) {
      std::println(stderr, "unmap-client: compositor is missing wp_color_manager_v1");
      return EXIT_FAILURE;
    }
    if (state.requestWindowsScrgb
        && (!state.colorManagerDone || !state.hasExtendedTargetVolume || !state.hasWindowsScrgb)) {
      std::println(stderr, "unmap-client: compositor is missing Wine scRGB color-management features");
      return EXIT_FAILURE;
    }
#ifdef WP_COLOR_MANAGER_V1_CREATE_WINDOWS_BT2100_SINCE_VERSION
    if (state.requestWindowsBt2100 && (!state.colorManagerDone || !state.hasWindowsBt2100)) {
      std::println(stderr, "unmap-client: compositor is missing Windows BT.2100 color-management support");
      return EXIT_FAILURE;
    }
#else
    if (state.requestWindowsBt2100) {
      std::println(stderr, "unmap-client: Windows BT.2100 requires wayland-protocols 1.49");
      return EXIT_FAILURE;
    }
#endif
    state.colorSurface = wp_color_manager_v1_get_surface(state.colorManager, colorTargetSurface);
    if (state.requestWindowsScrgb) {
      state.imageDescription = wp_color_manager_v1_create_windows_scrgb(state.colorManager);
#ifdef WP_COLOR_MANAGER_V1_CREATE_WINDOWS_BT2100_SINCE_VERSION
    } else if (state.requestWindowsBt2100) {
      state.imageDescription = wp_color_manager_v1_create_windows_bt2100(state.colorManager);
#endif
    } else {
      wp_image_description_creator_params_v1* params =
          wp_color_manager_v1_create_parametric_creator(state.colorManager);
      wp_image_description_creator_params_v1_set_tf_named(params, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
      wp_image_description_creator_params_v1_set_primaries_named(params, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
      state.imageDescription = wp_image_description_creator_params_v1_create(params);
    }
    wp_image_description_v1_add_listener(state.imageDescription, &kImageDescriptionListener, &state);
    while (!state.imageDescriptionReady && !state.imageDescriptionFailed && wl_display_roundtrip(state.display) >= 0) {
    }
    if (!state.imageDescriptionReady) {
      return EXIT_FAILURE;
    }
    if (!state.colorChildLifecycle) {
      wp_color_management_surface_v1_set_image_description(
          state.colorSurface, state.imageDescription, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL
      );
    }
  }
  if (state.colorChildSurface != nullptr && !state.colorChildLifecycle) {
    wl_surface_attach(state.colorChildSurface, state.colorChildBuffer.resource, 0, 0);
    wl_surface_damage_buffer(state.colorChildSurface, 0, 0, state.width, state.height);
    wl_surface_commit(state.colorChildSurface);
  }
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgSurfaceListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, state.title);
  if (state.appId != nullptr) {
    xdg_toplevel_set_app_id(state.toplevel, state.appId);
  }
  if (initialXdgTag != nullptr) {
    xdg_toplevel_tag_manager_v1_set_toplevel_tag(state.xdgTagManager, state.toplevel, initialXdgTag);
  }
  if (state.requestMaximized) {
    // A restored client state is requested before the initial commit. This lets
    // the compositor include it in the first configure instead of treating it
    // as a post-map maximize request.
    xdg_toplevel_set_maximized(state.toplevel);
  }
  if (transientSuite) {
    xdg_toplevel_set_parent(state.toplevel, transientParent.toplevel);
  }
  wl_surface_commit(state.surface);

  if (!remapOnStdin && !updateOnStdin) {
    while (wl_display_dispatch(state.display) >= 0) {
    }
  } else {
    pollfd sources[2] = {
        {.fd = wl_display_get_fd(state.display), .events = POLLIN, .revents = 0},
        {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
    };
    while (wl_display_dispatch_pending(state.display) >= 0) {
      wl_display_flush(state.display);
      int ready = 0;
      do {
        ready = poll(sources, 2, -1);
      } while (ready < 0 && errno == EINTR);
      if (ready < 0 || (sources[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        break;
      }
      if ((sources[1].revents & POLLIN) != 0) {
        char command = 0;
        if (read(STDIN_FILENO, &command, 1) > 0) {
          if (command == 'a') {
            if (!activateFromFile(state, activationTokenFile)) {
              return EXIT_FAILURE;
            }
            if (!state.mapped && remapOnStdin) {
              queueRemap(state);
            }
          } else if (command == 'c') {
            if (!activateWithClientToken(state)) {
              return EXIT_FAILURE;
            }
          } else if (state.mapped && updateOnStdin && !state.metadataUpdated) {
            if (updatedContentType != nullptr) {
              wp_content_type_v1_set_content_type(
                  state.contentType, static_cast<uint32_t>(parseContentType(updatedContentType))
              );
              wl_surface_commit(state.contentTypeSurface);
            }
            if (updatedXdgTag != nullptr) {
              xdg_toplevel_tag_manager_v1_set_toplevel_tag(state.xdgTagManager, state.toplevel, updatedXdgTag);
            }
            if (updatedTitle != nullptr) {
              xdg_toplevel_set_title(state.toplevel, updatedTitle);
            }
            wl_display_flush(state.display);
            state.metadataUpdated = true;
            std::println("{}", updatedXdgTag != nullptr ? "xdg-tag-updated" : "content-type-updated");
            std::fflush(stdout);
          } else if (!state.mapped && remapOnStdin) {
            queueRemap(state);
          }
        }
      }
      if ((sources[0].revents & POLLIN) != 0 && wl_display_dispatch(state.display) < 0) {
        break;
      }
    }
  }

  if (state.toplevel != nullptr) {
    xdg_toplevel_destroy(state.toplevel);
  }
  if (state.xdgSurface != nullptr) {
    xdg_surface_destroy(state.xdgSurface);
  }
  if (state.tearingControl != nullptr) {
    wp_tearing_control_v1_destroy(state.tearingControl);
  }
  if (state.colorSurface != nullptr) {
    wp_color_management_surface_v1_destroy(state.colorSurface);
  }
  if (state.imageDescription != nullptr) {
    wp_image_description_v1_destroy(state.imageDescription);
  }
  if (state.contentType != nullptr) {
    wp_content_type_v1_destroy(state.contentType);
  }
  if (state.colorChildSubsurface != nullptr) {
    wl_subsurface_destroy(state.colorChildSubsurface);
  }
  if (state.colorChildSurface != nullptr) {
    wl_surface_destroy(state.colorChildSurface);
  }
  if (state.surface != nullptr) {
    wl_surface_destroy(state.surface);
  }
  destroyAuxiliaryToplevel(transientUnrelated);
  destroyAuxiliaryToplevel(transientParent);
  if (state.colorManager != nullptr) {
    wp_color_manager_v1_destroy(state.colorManager);
  }
  if (state.tearingManager != nullptr) {
    wp_tearing_control_manager_v1_destroy(state.tearingManager);
  }
  if (state.contentTypeManager != nullptr) {
    wp_content_type_manager_v1_destroy(state.contentTypeManager);
  }
  if (state.xdgTagManager != nullptr) {
    xdg_toplevel_tag_manager_v1_destroy(state.xdgTagManager);
  }
  if (state.activation != nullptr) {
    xdg_activation_v1_destroy(state.activation);
  }
  if (state.keyboard != nullptr) {
    wl_keyboard_destroy(state.keyboard);
  }
  if (state.seat != nullptr) {
    wl_seat_destroy(state.seat);
  }
  destroyBuffer(state.buffer);
  destroyBuffer(state.colorChildBuffer);
  wl_display_disconnect(state.display);
  return EXIT_SUCCESS;
}
