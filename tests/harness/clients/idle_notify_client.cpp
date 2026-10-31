// Holds an ext-idle-notification and reports its idled and resumed events.

#include "ext-idle-notify-v1-client-protocol.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <wayland-client.h>

namespace {
  struct State {
    wl_seat* seat = nullptr;
    ext_idle_notifier_v1* notifier = nullptr;
  };

  void idleIdled(void*, ext_idle_notification_v1*) {
    std::println("idled");
    std::fflush(stdout);
  }
  void idleResumed(void*, ext_idle_notification_v1*) {
    std::println("resumed");
    std::fflush(stdout);
  }
  constexpr ext_idle_notification_v1_listener kIdleListener = {
      .idled = idleIdled,
      .resumed = idleResumed,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      state.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    } else if (std::strcmp(interface, ext_idle_notifier_v1_interface.name) == 0) {
      state.notifier =
          static_cast<ext_idle_notifier_v1*>(wl_registry_bind(registry, name, &ext_idle_notifier_v1_interface, 1));
    }
  }
  void registryRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener = {.global = registryGlobal, .global_remove = registryRemove};
} // namespace

int main(int argc, char** argv) {
  const uint32_t timeoutMs = argc > 1 ? static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10)) : 200;
  State state;
  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "idle-notify-client: cannot connect");
    return EXIT_FAILURE;
  }
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(display);
  if (state.seat == nullptr || state.notifier == nullptr) {
    std::println(stderr, "idle-notify-client: missing required global");
    return EXIT_FAILURE;
  }
  ext_idle_notification_v1* notification =
      ext_idle_notifier_v1_get_idle_notification(state.notifier, timeoutMs, state.seat);
  ext_idle_notification_v1_add_listener(notification, &kIdleListener, nullptr);
  wl_display_roundtrip(display);
  std::println("ready");
  std::fflush(stdout);

  while (wl_display_dispatch(display) >= 0) {
  }
  return EXIT_SUCCESS;
}
