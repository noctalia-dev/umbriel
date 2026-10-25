// Reports native color-management output and preferred descriptions as JSON lines. A mapped PQ surface also
// exercises native client metadata. Reading a label from stdin synchronizes events and reports notification counts.

#include "color-management-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <poll.h>
#include <print>
#include <ranges>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {
  struct Description {
    wp_image_description_v1* proxy = nullptr;
    const char* source = nullptr;
    unsigned generation = 0;
    uint64_t identity = 0;
    uint32_t readyVersion = 0;
    uint32_t transferFunction = 0;
    uint32_t primaries = 0;
    uint32_t minimum = 0;
    uint32_t maximum = 0;
    uint32_t reference = 0;
    uint32_t targetMinimum = 0;
    uint32_t targetMaximum = 0;
    uint32_t maxCll = 0;
    uint32_t maxFall = 0;
  };

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
    wl_output* output = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wp_color_manager_v1* manager = nullptr;
    wp_color_management_output_v1* colorOutput = nullptr;
    wp_color_management_surface_feedback_v1* feedback = nullptr;
    wp_color_management_surface_v1* colorSurface = nullptr;
    wp_image_description_v1* surfaceDescription = nullptr;
    wp_image_description_v1* gammaDescription = nullptr;
    wp_image_description_v1* pqDescription = nullptr;
    wp_image_description_v1* selectedDescription = nullptr;
    wl_surface* surface = nullptr;
    wl_surface* colorTarget = nullptr;
    wl_subsurface* colorSubsurface = nullptr;
    wl_callback* frame = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    uint32_t managerVersion = 0;
    uint32_t requestedVersion = 2;
    unsigned outputChanges = 0;
    unsigned preferredChanges = 0;
    unsigned frames = 0;
    int width = 320;
    int height = 240;
    bool ready = false;
    bool failed = false;
    bool mapped = false;
    bool running = true;
    bool desynchronized = false;
    bool tagged = true;
    bool compound = false;
    bool custom = false;
    bool contentLight = false;
    std::array<wp_image_description_v1*, 5> customDescriptions{};
    std::array<wp_image_description_v1*, 4> contentLightDescriptions{};
    uint32_t fillColor = 0xFF777777U;
    std::deque<Description> descriptions;
  };

  void informationDone(void* data, wp_image_description_info_v1* information) {
    const auto& description = *static_cast<Description*>(data);
    std::println(
        "{{\"event\":\"description\",\"source\":\"{}\",\"generation\":{},\"identity\":{},\"ready_version\":{},"
        "\"tf\":{},\"primaries\":{},"
        "\"min\":{},\"max\":{},\"reference\":{},\"target_min\":{},\"target_max\":{},\"max_cll\":{},\"max_fall\":{}}}",
        description.source, description.generation, description.identity, description.readyVersion,
        description.transferFunction, description.primaries, description.minimum, description.maximum,
        description.reference, description.targetMinimum, description.targetMaximum, description.maxCll,
        description.maxFall
    );
    std::fflush(stdout);
    wp_image_description_info_v1_destroy(information);
    wp_image_description_v1_destroy(description.proxy);
  }
  void informationIcc(void*, wp_image_description_info_v1*, int32_t fd, uint32_t) { close(fd); }
  void informationPrimaries(
      void*, wp_image_description_info_v1*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t
  ) {}
  void informationPrimariesNamed(void* data, wp_image_description_info_v1*, uint32_t primaries) {
    static_cast<Description*>(data)->primaries = primaries;
  }
  void informationTransferPower(void*, wp_image_description_info_v1*, uint32_t) {}
  void informationTransferNamed(void* data, wp_image_description_info_v1*, uint32_t transferFunction) {
    static_cast<Description*>(data)->transferFunction = transferFunction;
  }
  void informationLuminances(
      void* data, wp_image_description_info_v1*, uint32_t minimum, uint32_t maximum, uint32_t reference
  ) {
    auto& description = *static_cast<Description*>(data);
    description.minimum = minimum;
    description.maximum = maximum;
    description.reference = reference;
  }
  void informationTargetLuminance(void* data, wp_image_description_info_v1*, uint32_t minimum, uint32_t maximum) {
    auto& description = *static_cast<Description*>(data);
    description.targetMinimum = minimum;
    description.targetMaximum = maximum;
  }
  void informationMaxCll(void* data, wp_image_description_info_v1*, uint32_t maximum) {
    static_cast<Description*>(data)->maxCll = maximum;
  }
  void informationMaxFall(void* data, wp_image_description_info_v1*, uint32_t maximum) {
    static_cast<Description*>(data)->maxFall = maximum;
  }

  constexpr wp_image_description_info_v1_listener kInformationListener{
      .done = informationDone,
      .icc_file = informationIcc,
      .primaries = informationPrimaries,
      .primaries_named = informationPrimariesNamed,
      .tf_power = informationTransferPower,
      .tf_named = informationTransferNamed,
      .luminances = informationLuminances,
      .target_primaries = informationPrimaries,
      .target_luminance = informationTargetLuminance,
      .target_max_cll = informationMaxCll,
      .target_max_fall = informationMaxFall,
  };

  void descriptionFailed(void* data, wp_image_description_v1* proxy, uint32_t cause, const char* message) {
    const auto& description = *static_cast<Description*>(data);
    std::println(stderr, "color-feedback-client: {} description failed ({}): {}", description.source, cause, message);
    wp_image_description_v1_destroy(proxy);
  }
  void descriptionReady(void* data, wp_image_description_v1* proxy, uint32_t identity) {
    auto& description = *static_cast<Description*>(data);
    description.identity = identity;
    description.readyVersion = 1;
    wp_image_description_info_v1* information = wp_image_description_v1_get_information(proxy);
    wp_image_description_info_v1_add_listener(information, &kInformationListener, data);
  }
  void descriptionReady2(void* data, wp_image_description_v1* proxy, uint32_t high, uint32_t low) {
    descriptionReady(data, proxy, low);
    auto& description = *static_cast<Description*>(data);
    description.identity |= static_cast<uint64_t>(high) << 32;
    description.readyVersion = 2;
  }
  constexpr wp_image_description_v1_listener kDescriptionListener{
      .failed = descriptionFailed,
      .ready = descriptionReady,
      .ready2 = descriptionReady2,
  };

  void observeDescription(State& state, wp_image_description_v1* proxy, const char* source, unsigned generation) {
    state.descriptions.push_back({.proxy = proxy, .source = source, .generation = generation});
    wp_image_description_v1_add_listener(proxy, &kDescriptionListener, &state.descriptions.back());
  }
  void outputChanged(void* data, wp_color_management_output_v1* output) {
    auto& state = *static_cast<State*>(data);
    ++state.outputChanges;
    observeDescription(
        state, wp_color_management_output_v1_get_image_description(output), "output", state.outputChanges
    );
  }
  constexpr wp_color_management_output_v1_listener kOutputListener{.image_description_changed = outputChanged};

  void preferredChanged(void* data, wp_color_management_surface_feedback_v1* feedback, uint32_t) {
    auto& state = *static_cast<State*>(data);
    ++state.preferredChanges;
    observeDescription(
        state, wp_color_management_surface_feedback_v1_get_preferred(feedback), "preferred", state.preferredChanges
    );
    observeDescription(
        state, wp_color_management_surface_feedback_v1_get_preferred_parametric(feedback), "preferred_parametric",
        state.preferredChanges
    );
  }
  void preferredChanged2(void* data, wp_color_management_surface_feedback_v1* feedback, uint32_t, uint32_t) {
    preferredChanged(data, feedback, 0);
  }
  constexpr wp_color_management_surface_feedback_v1_listener kFeedbackListener{
      .preferred_changed = preferredChanged,
      .preferred_changed2 = preferredChanged2,
  };

  void supportedIntent(void*, wp_color_manager_v1*, uint32_t) {}
  void supportedFeature(void*, wp_color_manager_v1*, uint32_t feature) {
    std::println(R"({{"event":"feature","value":{}}})", feature);
  }
  void supportedTransfer(void*, wp_color_manager_v1*, uint32_t) {}
  void supportedPrimaries(void*, wp_color_manager_v1*, uint32_t) {}
  void managerDone(void*, wp_color_manager_v1*) { std::fflush(stdout); }
  constexpr wp_color_manager_v1_listener kManagerListener{
      .supported_intent = supportedIntent,
      .supported_feature = supportedFeature,
      .supported_tf_named = supportedTransfer,
      .supported_primaries_named = supportedPrimaries,
      .done = managerDone,
  };

  void wmBasePing(void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }
  constexpr xdg_wm_base_listener kWmBaseListener{.ping = wmBasePing};
  void
  outputGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
  void outputMode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
  constexpr wl_output_listener kWlOutputListener{
      .geometry = outputGeometry,
      .mode = outputMode,
      .done = nullptr,
      .scale = nullptr,
      .name = nullptr,
      .description = nullptr,
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
    } else if (std::strcmp(interface, wl_output_interface.name) == 0 && state.output == nullptr) {
      state.output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 1));
      wl_output_add_listener(state.output, &kWlOutputListener, &state);
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
      state.wmBase = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    } else if (std::strcmp(interface, wp_color_manager_v1_interface.name) == 0 && state.manager == nullptr) {
      state.managerVersion = std::min(version, state.requestedVersion);
      state.manager = static_cast<wp_color_manager_v1*>(
          wl_registry_bind(registry, name, &wp_color_manager_v1_interface, state.managerVersion)
      );
      wp_color_manager_v1_add_listener(state.manager, &kManagerListener, &state);
    }
  }
  void registryRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener{.global = registryGlobal, .global_remove = registryRemove};

  bool present(State& state) {
    const int stride = state.width * 4;
    const int size = stride * state.height;
    const int fd = memfd_create("umbriel-color-feedback", MFD_CLOEXEC);
    if (fd < 0) {
      return false;
    }
    if (ftruncate(fd, size) < 0) {
      close(fd);
      return false;
    }
    void* mapping = mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
      close(fd);
      return false;
    }
    auto* pixels = static_cast<uint32_t*>(mapping);
    std::ranges::fill_n(pixels, static_cast<size_t>(state.width) * static_cast<size_t>(state.height), state.fillColor);
    munmap(mapping, static_cast<size_t>(size));
    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, size);
    wl_buffer* buffer = wl_shm_pool_create_buffer(pool, 0, state.width, state.height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (state.tagged) {
      wp_color_management_surface_v1_set_image_description(
          state.colorSurface, state.selectedDescription, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL
      );
    }
    if (state.colorSubsurface != nullptr) {
      wl_surface_attach(state.colorTarget, buffer, 0, 0);
      wl_surface_damage_buffer(state.colorTarget, 0, 0, state.width, state.height);
      wl_surface_commit(state.colorTarget);
    }
    wl_surface_attach(state.surface, buffer, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, state.width, state.height);
    wl_surface_commit(state.surface);
    wl_buffer_destroy(buffer);
    if (!state.mapped) {
      state.mapped = true;
      std::println(R"({{"event":"mapped"}})");
      std::fflush(stdout);
    }
    return true;
  }
  void surfaceConfigure(void* data, xdg_surface* surface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(surface, serial);
    if (!present(state)) {
      state.failed = true;
      state.running = false;
    }
  }
  constexpr xdg_surface_listener kSurfaceListener{.configure = surfaceConfigure};
  void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
    auto& state = *static_cast<State*>(data);
    if (width > 0 && height > 0) {
      state.width = width;
      state.height = height;
    }
  }
  void toplevelClose(void* data, xdg_toplevel*) { static_cast<State*>(data)->running = false; }
  constexpr xdg_toplevel_listener kToplevelListener{
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = nullptr,
      .wm_capabilities = nullptr,
  };

  void surfaceDescriptionFailed(void* data, wp_image_description_v1*, uint32_t, const char* message) {
    auto& state = *static_cast<State*>(data);
    state.failed = true;
    std::println(stderr, "color-feedback-client: surface description failed: {}", message);
  }
  void surfaceDescriptionReady(void* data, wp_image_description_v1*, uint32_t) {
    static_cast<State*>(data)->ready = true;
  }
  void surfaceDescriptionReady2(void* data, wp_image_description_v1*, uint32_t, uint32_t) {
    static_cast<State*>(data)->ready = true;
  }
  constexpr wp_image_description_v1_listener kSurfaceDescriptionListener{
      .failed = surfaceDescriptionFailed,
      .ready = surfaceDescriptionReady,
      .ready2 = surfaceDescriptionReady2,
  };

  void frameDone(void* data, wl_callback* callback, uint32_t) {
    auto& state = *static_cast<State*>(data);
    wl_callback_destroy(callback);
    state.frame = nullptr;
    std::println(R"({{"event":"frame","count":{}}})", ++state.frames);
    std::fflush(stdout);
  }
  constexpr wl_callback_listener kFrameListener{.done = frameDone};

  void setRawPrimaries(wp_image_description_creator_params_v1* params, bool bt2020) {
    wp_image_description_creator_params_v1_set_primaries(
        params, bt2020 ? 708000 : 640000, bt2020 ? 292000 : 330000, bt2020 ? 170000 : 300000, bt2020 ? 797000 : 600000,
        bt2020 ? 131000 : 150000, bt2020 ? 46000 : 60000, 312700, 329000
    );
  }

  bool awaitSurfaceDescription(State& state, wp_image_description_v1* description) {
    state.ready = false;
    state.failed = false;
    wp_image_description_v1_add_listener(description, &kSurfaceDescriptionListener, &state);
    while (!state.ready && !state.failed && wl_display_dispatch(state.display) >= 0) {
    }
    return state.ready;
  }

  struct DescriptionResult {
    bool ready = false;
    bool failed = false;
    uint32_t cause = 0;
  };
  void resultFailed(void* data, wp_image_description_v1*, uint32_t cause, const char*) {
    auto& result = *static_cast<DescriptionResult*>(data);
    result.failed = true;
    result.cause = cause;
  }
  void resultReady(void* data, wp_image_description_v1*, uint32_t) {
    static_cast<DescriptionResult*>(data)->ready = true;
  }
  void resultReady2(void* data, wp_image_description_v1*, uint32_t, uint32_t) {
    static_cast<DescriptionResult*>(data)->ready = true;
  }
  constexpr wp_image_description_v1_listener kResultListener{
      .failed = resultFailed,
      .ready = resultReady,
      .ready2 = resultReady2,
  };

  bool expectProtocolError(State& state, const wl_interface* expectedInterface, uint32_t expectedCode) {
    if (wl_display_roundtrip(state.display) >= 0) {
      return false;
    }
    const wl_interface* interface = nullptr;
    uint32_t id = 0;
    const uint32_t code = wl_display_get_protocol_error(state.display, &interface, &id);
    return interface == expectedInterface && code == expectedCode;
  }
} // namespace

int main(int argc, char** argv) {
  State state;
  bool subsurface = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "custom") == 0) {
      state.custom = true;
      state.fillColor = 0xFF807060U;
    } else if (std::strcmp(argv[i], "content-light") == 0) {
      state.contentLight = true;
    } else if (std::strcmp(argv[i], "v1") == 0) {
      state.requestedVersion = 1;
    } else if (std::strcmp(argv[i], "v3") == 0) {
      state.requestedVersion = 3;
    } else if (std::strcmp(argv[i], "compound") == 0) {
      state.compound = true;
      state.fillColor = 0xFF101010U;
    } else if (std::strcmp(argv[i], "untagged") == 0) {
      state.tagged = false;
    } else if (std::strcmp(argv[i], "subsurface") == 0 || std::strcmp(argv[i], "subsurface-desync") == 0) {
      subsurface = true;
      state.desynchronized = std::strcmp(argv[i], "subsurface-desync") == 0;
    }
  }
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "color-feedback-client: no display");
    return 1;
  }
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  if (wl_display_roundtrip(state.display) < 0
      || wl_display_roundtrip(state.display) < 0
      || state.compositor == nullptr
      || state.shm == nullptr
      || state.output == nullptr
      || state.wmBase == nullptr
      || state.manager == nullptr) {
    std::println(stderr, "color-feedback-client: missing required globals");
    return 1;
  }
  std::println(R"({{"event":"manager","version":{}}})", state.managerVersion);
  if (argc >= 2 && std::strcmp(argv[1], "windows-scrgb") == 0) {
    auto* description = wp_color_manager_v1_create_windows_scrgb(state.manager);
    if (!awaitSurfaceDescription(state, description)) {
      std::println(stderr, "color-feedback-client: native Windows scRGB description was not ready");
      return 1;
    }
    std::println(R"({{"event":"windows_scrgb_ready"}})");
    wp_image_description_v1_destroy(description);
    wl_display_disconnect(state.display);
    return 0;
  }
  if (argc == 2 && std::strncmp(argv[1], "params-", 7) == 0) {
    const char* mode = argv[1] + 7;
    auto* params = wp_color_manager_v1_create_parametric_creator(state.manager);
    wp_image_description_creator_params_v1_set_tf_named(params, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
    uint32_t expected = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET;
    if (std::strcmp(mode, "named-raw") == 0) {
      wp_image_description_creator_params_v1_set_primaries_named(params, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
      setRawPrimaries(params, true);
    } else if (std::strcmp(mode, "raw-named") == 0) {
      setRawPrimaries(params, true);
      wp_image_description_creator_params_v1_set_primaries_named(params, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
    } else if (std::strcmp(mode, "raw-raw") == 0) {
      setRawPrimaries(params, true);
      setRawPrimaries(params, false);
    } else if (std::strcmp(mode, "luminances-twice") == 0) {
      wp_image_description_creator_params_v1_set_luminances(params, 2000, 80, 80);
      wp_image_description_creator_params_v1_set_luminances(params, 2000, 160, 80);
    } else if (std::strcmp(mode, "invalid-maximum") == 0) {
      wp_image_description_creator_params_v1_set_luminances(params, 1000000, 100, 203);
      expected = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE;
    } else if (std::strcmp(mode, "invalid-reference") == 0) {
      wp_image_description_creator_params_v1_set_luminances(params, 1000000, 200, 100);
      expected = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE;
    } else if (std::strcmp(mode, "degenerate") == 0) {
      wp_image_description_creator_params_v1_set_primaries(params, 0, 0, 0, 0, 0, 0, 312700, 329000);
      auto* description = wp_image_description_creator_params_v1_create(params);
      DescriptionResult result;
      wp_image_description_v1_add_listener(description, &kResultListener, &result);
      while (!result.ready && !result.failed && wl_display_dispatch(state.display) >= 0) {
      }
      if (!result.failed
          || result.cause != WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED
          || wl_display_roundtrip(state.display) < 0) {
        std::println(stderr, "color-feedback-client: degenerate primaries did not fail gracefully");
        return 1;
      }
      std::println(R"({{"event":"description_failed","cause":{}}})", result.cause);
      wp_image_description_v1_destroy(description);
      wl_display_disconnect(state.display);
      return 0;
    } else {
      std::println(stderr, "color-feedback-client: unknown parameter case: {}", mode);
      return 1;
    }
    if (!expectProtocolError(state, &wp_image_description_creator_params_v1_interface, expected)) {
      std::println(stderr, "color-feedback-client: {} did not produce protocol error {}", mode, expected);
      return 1;
    }
    std::println(R"({{"event":"parameter_error","case":"{}","code":{}}})", mode, expected);
    wl_display_disconnect(state.display);
    return 0;
  }
  if (argc == 2 && (std::strcmp(argv[1], "duplicate") == 0 || std::strcmp(argv[1], "inert") == 0)) {
    state.surface = wl_compositor_create_surface(state.compositor);
    state.colorSurface = wp_color_manager_v1_get_surface(state.manager, state.surface);
    if (std::strcmp(argv[1], "duplicate") == 0) {
      wp_color_manager_v1_get_surface(state.manager, state.surface);
      if (expectProtocolError(state, &wp_color_manager_v1_interface, WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS)) {
        std::println(R"({{"event":"surface_exists"}})");
        return 0;
      }
    } else {
      wl_surface_destroy(state.surface);
      wp_color_management_surface_v1_unset_image_description(state.colorSurface);
      if (expectProtocolError(
              state, &wp_color_management_surface_v1_interface, WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT
          )) {
        std::println(R"({{"event":"surface_inert"}})");
        return 0;
      }
    }
    std::println(stderr, "color-feedback-client: {} did not produce the expected surface protocol error", argv[1]);
    return 1;
  }
  state.colorOutput = wp_color_manager_v1_get_output(state.manager, state.output);
  wp_color_management_output_v1_add_listener(state.colorOutput, &kOutputListener, &state);
  observeDescription(state, wp_color_management_output_v1_get_image_description(state.colorOutput), "output", 0);
  state.surface = wl_compositor_create_surface(state.compositor);
  state.colorTarget = state.surface;
  if (subsurface) {
    if (state.subcompositor == nullptr) {
      std::println(stderr, "color-feedback-client: missing subcompositor");
      return 1;
    }
    state.colorTarget = wl_compositor_create_surface(state.compositor);
    state.colorSubsurface = wl_subcompositor_get_subsurface(state.subcompositor, state.colorTarget, state.surface);
    if (state.desynchronized) {
      wl_subsurface_set_desync(state.colorSubsurface);
    }
  }
  state.feedback = wp_color_manager_v1_get_surface_feedback(state.manager, state.surface);
  wp_color_management_surface_feedback_v1_add_listener(state.feedback, &kFeedbackListener, &state);
  observeDescription(state, wp_color_management_surface_feedback_v1_get_preferred(state.feedback), "preferred", 0);
  observeDescription(
      state, wp_color_management_surface_feedback_v1_get_preferred_parametric(state.feedback), "preferred_parametric", 0
  );
  state.colorSurface = wp_color_manager_v1_get_surface(state.manager, state.colorTarget);
  wp_image_description_creator_params_v1* params = wp_color_manager_v1_create_parametric_creator(state.manager);
  wp_image_description_creator_params_v1_set_tf_named(
      params,
      state.compound ? WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4
                     : WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ
  );
  wp_image_description_creator_params_v1_set_primaries_named(
      params, state.compound ? WP_COLOR_MANAGER_V1_PRIMARIES_SRGB : WP_COLOR_MANAGER_V1_PRIMARIES_BT2020
  );
  state.surfaceDescription = wp_image_description_creator_params_v1_create(params);
  state.selectedDescription = state.surfaceDescription;
  wp_image_description_v1_add_listener(state.surfaceDescription, &kSurfaceDescriptionListener, &state);
  while (!state.ready && !state.failed && wl_display_dispatch(state.display) >= 0) {
  }
  if (!state.ready) {
    return 1;
  }
  if (state.compound) {
    state.ready = false;
    params = wp_color_manager_v1_create_parametric_creator(state.manager);
    wp_image_description_creator_params_v1_set_tf_named(params, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
    wp_image_description_creator_params_v1_set_primaries_named(params, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
    state.gammaDescription = wp_image_description_creator_params_v1_create(params);
    wp_image_description_v1_add_listener(state.gammaDescription, &kSurfaceDescriptionListener, &state);
    while (!state.ready && !state.failed && wl_display_dispatch(state.display) >= 0) {
    }
    if (!state.ready) {
      return 1;
    }
    state.ready = false;
    params = wp_color_manager_v1_create_parametric_creator(state.manager);
    wp_image_description_creator_params_v1_set_tf_named(params, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    wp_image_description_creator_params_v1_set_primaries_named(params, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    state.pqDescription = wp_image_description_creator_params_v1_create(params);
    wp_image_description_v1_add_listener(state.pqDescription, &kSurfaceDescriptionListener, &state);
    while (!state.ready && !state.failed && wl_display_dispatch(state.display) >= 0) {
    }
    if (!state.ready) {
      return 1;
    }
  }
  if (state.custom) {
    for (size_t i = 0; i < state.customDescriptions.size(); ++i) {
      params = wp_color_manager_v1_create_parametric_creator(state.manager);
      // Setting luminances before the transfer function must preserve the explicit values.
      if (i >= 2) {
        wp_image_description_creator_params_v1_set_luminances(params, 0, i == 2 ? 80 : 160, i == 4 ? 160 : 80);
      }
      wp_image_description_creator_params_v1_set_tf_named(params, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
      if (i == 0) {
        wp_image_description_creator_params_v1_set_primaries_named(params, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
      } else {
        setRawPrimaries(params, i == 1);
      }
      state.customDescriptions[i] = wp_image_description_creator_params_v1_create(params);
      if (!awaitSurfaceDescription(state, state.customDescriptions[i])) {
        return 1;
      }
    }
    wp_image_description_v1_destroy(state.surfaceDescription);
    state.surfaceDescription = state.customDescriptions[0];
    state.selectedDescription = state.surfaceDescription;
  }
  if (state.contentLight) {
    constexpr std::array<uint32_t, 4> peaks{203, 1000, 0, 203};
    for (size_t i = 0; i < peaks.size(); ++i) {
      params = wp_color_manager_v1_create_parametric_creator(state.manager);
      wp_image_description_creator_params_v1_set_tf_named(params, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
      wp_image_description_creator_params_v1_set_primaries_named(params, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
      wp_image_description_creator_params_v1_set_luminances(params, 0, 10000, 203);
      if (i == 3) {
        wp_image_description_creator_params_v1_set_mastering_luminance(params, 0, 1000);
      }
      wp_image_description_creator_params_v1_set_max_cll(params, peaks[i]);
      wp_image_description_creator_params_v1_set_max_fall(params, 203);
      state.contentLightDescriptions[i] = wp_image_description_creator_params_v1_create(params);
      if (!awaitSurfaceDescription(state, state.contentLightDescriptions[i])) {
        return 1;
      }
    }
    wp_image_description_v1_destroy(state.surfaceDescription);
    state.surfaceDescription = state.contentLightDescriptions[0];
    state.selectedDescription = state.surfaceDescription;
  }
  if (state.requestedVersion == 1) {
    observeDescription(state, wp_color_management_surface_feedback_v1_get_preferred(state.feedback), "preferred", 0);
    observeDescription(
        state, wp_color_management_surface_feedback_v1_get_preferred_parametric(state.feedback), "preferred_parametric",
        0
    );
    for (int i = 0; i < 3; ++i) {
      if (wl_display_roundtrip(state.display) < 0) {
        return 1;
      }
    }
    wl_display_disconnect(state.display);
    return 0;
  }
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kSurfaceListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, "color-feedback-client");
  xdg_toplevel_set_app_id(state.toplevel, "color-feedback-client");
  xdg_toplevel_set_fullscreen(state.toplevel, state.output);
  wl_surface_commit(state.surface);

  bool stdinOpen = true;
  while (state.running && wl_display_dispatch_pending(state.display) >= 0) {
    if (wl_display_flush(state.display) < 0 && errno != EAGAIN) {
      return 1;
    }
    pollfd fds[] = {
        {.fd = wl_display_get_fd(state.display), .events = POLLIN, .revents = 0},
        {.fd = stdinOpen ? STDIN_FILENO : -1, .events = POLLIN, .revents = 0}
    };
    if (poll(fds, 2, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      return 1;
    }
    if ((fds[0].revents & (POLLERR | POLLHUP)) != 0
        || ((fds[0].revents & POLLIN) != 0 && wl_display_dispatch(state.display) < 0)) {
      return 1;
    }
    if ((fds[1].revents & (POLLIN | POLLHUP)) != 0) {
      char label[128];
      if (std::fgets(label, sizeof(label), stdin) == nullptr) {
        stdinOpen = false;
        continue;
      }
      label[std::strcspn(label, "\r\n")] = '\0';
      if (std::strcmp(label, "commit") == 0 || std::strcmp(label, "frame") == 0) {
        if (std::strcmp(label, "frame") == 0 && state.frame == nullptr) {
          state.frame = wl_surface_frame(state.colorTarget);
          wl_callback_add_listener(state.frame, &kFrameListener, &state);
        }
        wl_surface_commit(state.colorTarget);
        if (state.colorSubsurface != nullptr && !state.desynchronized) {
          wl_surface_commit(state.surface);
        }
      } else if (std::strcmp(label, "windowed") == 0 || std::strcmp(label, "fullscreen") == 0) {
        if (std::strcmp(label, "fullscreen") == 0) {
          xdg_toplevel_set_fullscreen(state.toplevel, state.output);
        } else {
          xdg_toplevel_unset_fullscreen(state.toplevel);
        }
        wl_surface_commit(state.surface);
      } else if (state.contentLight && std::strncmp(label, "content ", 8) == 0) {
        unsigned index = 0;
        char trailing = '\0';
        if (std::sscanf(label + 8, "%u%c", &index, &trailing) != 1 || index >= state.contentLightDescriptions.size()) {
          std::println(stderr, "color-feedback-client: invalid content description: {}", label);
          return 1;
        }
        state.tagged = true;
        state.selectedDescription = state.contentLightDescriptions[index];
        wp_color_management_surface_v1_set_image_description(
            state.colorSurface, state.selectedDescription, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL
        );
        wl_surface_commit(state.colorTarget);
        if (state.colorSubsurface != nullptr && !state.desynchronized) {
          wl_surface_commit(state.surface);
        }
      } else if (state.custom && std::strncmp(label, "custom ", 7) == 0) {
        unsigned index = 0;
        char trailing = '\0';
        if (std::sscanf(label + 7, "%u%c", &index, &trailing) != 1 || index >= state.customDescriptions.size()) {
          std::println(stderr, "color-feedback-client: invalid custom description: {}", label);
          return 1;
        }
        state.tagged = true;
        state.selectedDescription = state.customDescriptions[index];
        wp_color_management_surface_v1_set_image_description(
            state.colorSurface, state.selectedDescription, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL
        );
        wl_surface_commit(state.colorTarget);
        if (state.colorSubsurface != nullptr && !state.desynchronized) {
          wl_surface_commit(state.surface);
        }
      } else if (std::strcmp(label, "unset") == 0) {
        state.tagged = false;
        wp_color_management_surface_v1_unset_image_description(state.colorSurface);
        wl_surface_commit(state.colorTarget);
        if (state.colorSubsurface != nullptr && !state.desynchronized) {
          wl_surface_commit(state.surface);
        }
      } else if (
          std::strcmp(label, "tag") == 0
          || (state.gammaDescription != nullptr && std::strcmp(label, "gamma22") == 0)
          || (state.pqDescription != nullptr && std::strcmp(label, "pq") == 0)
      ) {
        state.tagged = true;
        if (std::strcmp(label, "gamma22") == 0) {
          state.selectedDescription = state.gammaDescription;
        } else if (std::strcmp(label, "pq") == 0) {
          state.selectedDescription = state.pqDescription;
        } else {
          state.selectedDescription = state.surfaceDescription;
        }
        wp_color_management_surface_v1_set_image_description(
            state.colorSurface, state.selectedDescription, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL
        );
        wl_surface_commit(state.colorTarget);
        if (state.colorSubsurface != nullptr && !state.desynchronized) {
          wl_surface_commit(state.surface);
        }
      } else if (state.colorSubsurface != nullptr && std::strcmp(label, "cache") == 0) {
        wl_subsurface_set_sync(state.colorSubsurface);
        wp_color_management_surface_v1_unset_image_description(state.colorSurface);
        wl_surface_commit(state.colorTarget);
      } else if (state.colorSubsurface != nullptr && std::strcmp(label, "parent") == 0) {
        wp_color_management_surface_v1_set_image_description(
            state.colorSurface, state.selectedDescription, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL
        );
        wl_surface_commit(state.surface);
      } else if (state.colorSubsurface != nullptr && std::strcmp(label, "apply") == 0) {
        wl_surface_commit(state.colorTarget);
        wl_surface_commit(state.surface);
      } else if (std::strcmp(label, "destroy") == 0 || std::strcmp(label, "destroy-only") == 0) {
        state.tagged = false;
        wp_color_management_surface_v1_destroy(state.colorSurface);
        state.colorSurface = nullptr;
        if (std::strcmp(label, "destroy") == 0) {
          wl_surface_commit(state.colorTarget);
          if (state.colorSubsurface != nullptr && !state.desynchronized) {
            wl_surface_commit(state.surface);
          }
        }
      } else if (std::strcmp(label, "recreate") == 0) {
        state.tagged = true;
        state.selectedDescription = state.surfaceDescription;
        state.colorSurface = wp_color_manager_v1_get_surface(state.manager, state.colorTarget);
        wp_color_management_surface_v1_set_image_description(
            state.colorSurface, state.surfaceDescription, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL
        );
        wl_surface_commit(state.colorTarget);
        if (state.colorSubsurface != nullptr && !state.desynchronized) {
          wl_surface_commit(state.surface);
        }
      } else if (std::strncmp(label, "title ", 6) == 0) {
        xdg_toplevel_set_title(state.toplevel, label + 6);
      } else if (std::strncmp(label, "color ", 6) == 0) {
        unsigned color = 0;
        char trailing = '\0';
        if (std::sscanf(label + 6, "%x%c", &color, &trailing) != 1) {
          std::println(stderr, "color-feedback-client: invalid color: {}", label);
          return 1;
        }
        state.fillColor = static_cast<uint32_t>(color);
        if (!present(state)) {
          return 1;
        }
      }
      observeDescription(
          state, wp_color_management_output_v1_get_image_description(state.colorOutput), "output", state.outputChanges
      );
      observeDescription(
          state, wp_color_management_surface_feedback_v1_get_preferred(state.feedback), "preferred",
          state.preferredChanges
      );
      observeDescription(
          state, wp_color_management_surface_feedback_v1_get_preferred_parametric(state.feedback),
          "preferred_parametric", state.preferredChanges
      );
      for (int i = 0; i < 3; ++i) {
        if (wl_display_roundtrip(state.display) < 0) {
          return 1;
        }
      }
      std::println(
          R"({{"event":"snapshot","label":"{}","output_changes":{},"preferred_changes":{}}})", label,
          state.outputChanges, state.preferredChanges
      );
      std::fflush(stdout);
    }
  }
  wl_display_disconnect(state.display);
  return state.failed ? 1 : 0;
}
