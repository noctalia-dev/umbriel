#include <cstring>
#include <print>
#include <string_view>
#include <wayland-client.h>

namespace {
  struct State {
    std::string_view wanted;
    bool found = false;
  };

  void handleGlobal(void* data, wl_registry*, uint32_t, const char* interface, uint32_t) {
    auto* state = static_cast<State*>(data);
    if (state->wanted == interface) {
      state->found = true;
    }
  }

  void handleGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener{
      .global = handleGlobal,
      .global_remove = handleGlobalRemove,
  };
} // namespace

int main(int argc, char** argv) {
  if (argc != 3 || (std::strcmp(argv[2], "present") != 0 && std::strcmp(argv[2], "absent") != 0)) {
    std::println(stderr, "usage: global-client INTERFACE present|absent");
    return 2;
  }

  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "global-client: cannot connect to WAYLAND_DISPLAY");
    return 2;
  }

  State state{.wanted = argv[1]};
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  const bool roundtripOk = wl_display_roundtrip(display) >= 0;
  wl_registry_destroy(registry);
  wl_display_disconnect(display);

  if (!roundtripOk) {
    std::println(stderr, "global-client: registry roundtrip failed");
    return 2;
  }

  const bool expected = std::strcmp(argv[2], "present") == 0;
  if (state.found != expected) {
    std::println(
        stderr, "global-client: {} was {}, expected {}", state.wanted, state.found ? "present" : "absent",
        expected ? "present" : "absent"
    );
    return 1;
  }
  return 0;
}
