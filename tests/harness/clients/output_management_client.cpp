#include "wlr-output-management-unstable-v1-client-protocol.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <deque>
#include <print>
#include <string>
#include <string_view>
#include <wayland-client.h>

namespace {
  struct Mode {
    zwlr_output_mode_v1* proxy = nullptr;
    bool preferred = false;
  };

  struct Head {
    zwlr_output_head_v1* proxy = nullptr;
    std::string name;
    bool enabled = false;
    int32_t x = 0;
    int32_t y = 0;
    int32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;
    double scale = 1.0;
    int32_t adaptiveSync = -1;
    zwlr_output_mode_v1* currentMode = nullptr;
    std::deque<Mode> modes;
  };

  struct State {
    wl_registry* registry = nullptr;
    zwlr_output_manager_v1* manager = nullptr;
    uint32_t managerVersion = 0;
    uint32_t serial = 0;
    std::deque<Head> heads;
    bool managerDone = false;
    bool configurationDone = false;
    bool configurationSucceeded = false;
  };

  void modeSize(void*, zwlr_output_mode_v1*, int32_t, int32_t) {}
  void modeRefresh(void*, zwlr_output_mode_v1*, int32_t) {}
  void modePreferred(void* data, zwlr_output_mode_v1*) { static_cast<Mode*>(data)->preferred = true; }
  void modeFinished(void*, zwlr_output_mode_v1*) {}

  constexpr zwlr_output_mode_v1_listener kModeListener{
      .size = modeSize,
      .refresh = modeRefresh,
      .preferred = modePreferred,
      .finished = modeFinished,
  };

  void headName(void* data, zwlr_output_head_v1*, const char* name) {
    static_cast<Head*>(data)->name = name != nullptr ? name : "";
  }
  void headDescription(void*, zwlr_output_head_v1*, const char*) {}
  void headPhysicalSize(void*, zwlr_output_head_v1*, int32_t, int32_t) {}
  void headMode(void* data, zwlr_output_head_v1*, zwlr_output_mode_v1* mode) {
    auto* head = static_cast<Head*>(data);
    head->modes.push_back({.proxy = mode});
    zwlr_output_mode_v1_add_listener(mode, &kModeListener, &head->modes.back());
  }
  void headEnabled(void* data, zwlr_output_head_v1*, int32_t enabled) {
    static_cast<Head*>(data)->enabled = enabled != 0;
  }
  void headCurrentMode(void* data, zwlr_output_head_v1*, zwlr_output_mode_v1* mode) {
    static_cast<Head*>(data)->currentMode = mode;
  }
  void headPosition(void* data, zwlr_output_head_v1*, int32_t x, int32_t y) {
    auto* head = static_cast<Head*>(data);
    head->x = x;
    head->y = y;
  }
  void headTransform(void* data, zwlr_output_head_v1*, int32_t transform) {
    static_cast<Head*>(data)->transform = transform;
  }
  void headScale(void* data, zwlr_output_head_v1*, wl_fixed_t scale) {
    static_cast<Head*>(data)->scale = wl_fixed_to_double(scale);
  }
  void headFinished(void*, zwlr_output_head_v1*) {}
  void headMake(void*, zwlr_output_head_v1*, const char*) {}
  void headModel(void*, zwlr_output_head_v1*, const char*) {}
  void headSerialNumber(void*, zwlr_output_head_v1*, const char*) {}
  void headAdaptiveSync(void* data, zwlr_output_head_v1*, uint32_t adaptiveSync) {
    static_cast<Head*>(data)->adaptiveSync = static_cast<int32_t>(adaptiveSync);
  }

  constexpr zwlr_output_head_v1_listener kHeadListener{
      .name = headName,
      .description = headDescription,
      .physical_size = headPhysicalSize,
      .mode = headMode,
      .enabled = headEnabled,
      .current_mode = headCurrentMode,
      .position = headPosition,
      .transform = headTransform,
      .scale = headScale,
      .finished = headFinished,
      .make = headMake,
      .model = headModel,
      .serial_number = headSerialNumber,
      .adaptive_sync = headAdaptiveSync,
  };

  void managerHead(void* data, zwlr_output_manager_v1*, zwlr_output_head_v1* proxy) {
    auto* state = static_cast<State*>(data);
    state->heads.emplace_back();
    state->heads.back().proxy = proxy;
    zwlr_output_head_v1_add_listener(proxy, &kHeadListener, &state->heads.back());
  }
  void managerDone(void* data, zwlr_output_manager_v1*, uint32_t serial) {
    auto* state = static_cast<State*>(data);
    state->serial = serial;
    state->managerDone = true;
  }
  void managerFinished(void* data, zwlr_output_manager_v1*) { static_cast<State*>(data)->managerDone = true; }

  constexpr zwlr_output_manager_v1_listener kManagerListener{
      .head = managerHead,
      .done = managerDone,
      .finished = managerFinished,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto* state = static_cast<State*>(data);
    if (std::strcmp(interface, zwlr_output_manager_v1_interface.name) != 0) {
      return;
    }
    state->managerVersion = std::min(version, 4U);
    state->manager = static_cast<zwlr_output_manager_v1*>(
        wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, state->managerVersion)
    );
    zwlr_output_manager_v1_add_listener(state->manager, &kManagerListener, state);
  }
  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener{
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };

  void configurationSucceeded(void* data, zwlr_output_configuration_v1*) {
    auto* state = static_cast<State*>(data);
    state->configurationSucceeded = true;
    state->configurationDone = true;
  }
  void configurationFailed(void* data, zwlr_output_configuration_v1*) {
    static_cast<State*>(data)->configurationDone = true;
  }
  void configurationCancelled(void* data, zwlr_output_configuration_v1*) {
    static_cast<State*>(data)->configurationDone = true;
  }

  constexpr zwlr_output_configuration_v1_listener kConfigurationListener{
      .succeeded = configurationSucceeded,
      .failed = configurationFailed,
      .cancelled = configurationCancelled,
  };

  zwlr_output_mode_v1* selectedMode(const Head& head) {
    if (head.currentMode != nullptr) {
      return head.currentMode;
    }
    const auto preferred = std::ranges::find_if(head.modes, [](const Mode& mode) { return mode.preferred; });
    return preferred != head.modes.end() ? preferred->proxy : (head.modes.empty() ? nullptr : head.modes.front().proxy);
  }

  void destroyState(State& state) {
    for (Head& head : state.heads) {
      for (Mode& mode : head.modes) {
        zwlr_output_mode_v1_destroy(mode.proxy);
      }
      zwlr_output_head_v1_destroy(head.proxy);
    }
    if (state.manager != nullptr) {
      zwlr_output_manager_v1_destroy(state.manager);
    }
    if (state.registry != nullptr) {
      wl_registry_destroy(state.registry);
    }
  }
} // namespace

int main(int argc, char** argv) {
  const bool test = argc >= 4 && std::string_view(argv[1]) == "test";
  const bool apply = argc >= 4 && std::string_view(argv[1]) == "apply";
  const bool enable = argc >= 4 && std::string_view(argv[2]) == "enable";
  const bool disable = argc >= 4 && std::string_view(argv[2]) == "disable";
  if (argc != 4 && argc != 6) {
    std::println(stderr, "usage: output-management-client <test|apply> <enable|disable> OUTPUT [X Y]");
    return 2;
  }
  if ((!test && !apply) || (!enable && !disable) || (argc == 6 && !enable)) {
    std::println(stderr, "output-management-client: invalid operation");
    return 2;
  }

  int32_t requestedX = 0;
  int32_t requestedY = 0;
  if (argc == 6) {
    const auto parseCoordinate = [](const char* text, int32_t& value) {
      const char* end = text + std::strlen(text);
      const auto [next, error] = std::from_chars(text, end, value);
      return error == std::errc() && next == end;
    };
    if (!parseCoordinate(argv[4], requestedX) || !parseCoordinate(argv[5], requestedY)) {
      std::println(stderr, "output-management-client: position must contain two integers");
      return 2;
    }
  }

  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "output-management-client: cannot connect to WAYLAND_DISPLAY");
    return 2;
  }

  State state;
  state.registry = wl_display_get_registry(display);
  wl_registry_add_listener(state.registry, &kRegistryListener, &state);
  for (int roundtrip = 0; roundtrip < 4 && !state.managerDone; ++roundtrip) {
    if (wl_display_roundtrip(display) < 0) {
      break;
    }
  }
  if (state.manager == nullptr || !state.managerDone) {
    std::println(stderr, "output-management-client: output manager did not enumerate heads");
    destroyState(state);
    wl_display_disconnect(display);
    return 1;
  }

  const std::string_view targetName(argv[3]);
  const auto target =
      std::ranges::find_if(state.heads, [targetName](const Head& head) { return head.name == targetName; });
  if (target == state.heads.end()) {
    std::println(stderr, "output-management-client: unknown output: {}", targetName);
    destroyState(state);
    wl_display_disconnect(display);
    return 1;
  }

  zwlr_output_configuration_v1* configuration =
      zwlr_output_manager_v1_create_configuration(state.manager, state.serial);
  zwlr_output_configuration_v1_add_listener(configuration, &kConfigurationListener, &state);
  for (const Head& head : state.heads) {
    const bool targetHead = &head == &*target;
    const bool requestedEnabled = targetHead ? enable : head.enabled;
    if (!requestedEnabled) {
      zwlr_output_configuration_v1_disable_head(configuration, head.proxy);
      continue;
    }

    zwlr_output_configuration_head_v1* configured = zwlr_output_configuration_v1_enable_head(configuration, head.proxy);
    if (zwlr_output_mode_v1* mode = selectedMode(head)) {
      zwlr_output_configuration_head_v1_set_mode(configured, mode);
    } else {
      // The harness headless backend uses this mode and may expose it as a
      // custom mode rather than an advertised mode.
      zwlr_output_configuration_head_v1_set_custom_mode(configured, 1280, 720, 0);
    }
    const int32_t x = targetHead && argc == 6 ? requestedX : head.x;
    const int32_t y = targetHead && argc == 6 ? requestedY : head.y;
    zwlr_output_configuration_head_v1_set_position(configured, x, y);
    zwlr_output_configuration_head_v1_set_transform(configured, head.transform);
    zwlr_output_configuration_head_v1_set_scale(configured, wl_fixed_from_double(head.scale));
    if (state.managerVersion >= 4 && head.adaptiveSync >= 0) {
      zwlr_output_configuration_head_v1_set_adaptive_sync(configured, static_cast<uint32_t>(head.adaptiveSync));
    }
  }

  if (test) {
    zwlr_output_configuration_v1_test(configuration);
  } else {
    zwlr_output_configuration_v1_apply(configuration);
  }
  while (!state.configurationDone && wl_display_dispatch(display) >= 0) {
  }

  zwlr_output_configuration_v1_destroy(configuration);
  const bool succeeded = state.configurationSucceeded;
  destroyState(state);
  wl_display_disconnect(display);
  if (!succeeded) {
    std::println(stderr, "output-management-client: configuration failed");
    return 1;
  }
  std::println("configuration succeeded");
  return 0;
}
