#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <vector>
#include <wayland-client.h>

namespace {
  struct Output {
    wl_output* resource = nullptr;
    uint32_t version = 0;
    std::string name;
  };

  struct Toplevel {
    zwlr_foreign_toplevel_handle_v1* handle = nullptr;
    std::string title;
    std::vector<wl_output*> outputs;
    bool closed = false;
  };

  struct State {
    wl_registry* registry = nullptr;
    uint32_t managerName = 0;
    uint32_t managerVersion = 0;
    zwlr_foreign_toplevel_manager_v1* manager = nullptr;
    wl_seat* seat = nullptr;
    uint32_t seatVersion = 0;
    std::vector<std::unique_ptr<Output>> outputs;
    std::vector<std::unique_ptr<Toplevel>> toplevels;
    bool finished = false;
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

  constexpr wl_output_listener kOutputListener{
      .geometry = outputGeometry,
      .mode = outputMode,
      .done = outputDone,
      .scale = outputScale,
      .name = outputName,
      .description = outputDescription,
  };

  void toplevelTitle(void* data, zwlr_foreign_toplevel_handle_v1*, const char* title) {
    static_cast<Toplevel*>(data)->title = title != nullptr ? title : "";
  }
  void toplevelAppId(void*, zwlr_foreign_toplevel_handle_v1*, const char*) {}
  void toplevelOutputEnter(void* data, zwlr_foreign_toplevel_handle_v1*, wl_output* output) {
    auto& outputs = static_cast<Toplevel*>(data)->outputs;
    if (std::ranges::find(outputs, output) == outputs.end()) {
      outputs.push_back(output);
    }
  }
  void toplevelOutputLeave(void* data, zwlr_foreign_toplevel_handle_v1*, wl_output* output) {
    auto& outputs = static_cast<Toplevel*>(data)->outputs;
    std::erase(outputs, output);
  }
  void toplevelState(void*, zwlr_foreign_toplevel_handle_v1*, wl_array*) {}
  void toplevelDone(void*, zwlr_foreign_toplevel_handle_v1*) {}
  void toplevelClosed(void* data, zwlr_foreign_toplevel_handle_v1*) { static_cast<Toplevel*>(data)->closed = true; }
  void toplevelParent(void*, zwlr_foreign_toplevel_handle_v1*, zwlr_foreign_toplevel_handle_v1*) {}

  constexpr zwlr_foreign_toplevel_handle_v1_listener kToplevelListener{
      .title = toplevelTitle,
      .app_id = toplevelAppId,
      .output_enter = toplevelOutputEnter,
      .output_leave = toplevelOutputLeave,
      .state = toplevelState,
      .done = toplevelDone,
      .closed = toplevelClosed,
      .parent = toplevelParent,
  };

  void managerToplevel(void* data, zwlr_foreign_toplevel_manager_v1*, zwlr_foreign_toplevel_handle_v1* handle) {
    auto& state = *static_cast<State*>(data);
    auto toplevel = std::make_unique<Toplevel>();
    toplevel->handle = handle;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &kToplevelListener, toplevel.get());
    state.toplevels.push_back(std::move(toplevel));
  }

  void managerFinished(void* data, zwlr_foreign_toplevel_manager_v1*) { static_cast<State*>(data)->finished = true; }

  constexpr zwlr_foreign_toplevel_manager_v1_listener kManagerListener{
      .toplevel = managerToplevel,
      .finished = managerFinished,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::string_view(interface) == zwlr_foreign_toplevel_manager_v1_interface.name) {
      state.managerName = name;
      state.managerVersion = version;
    } else if (std::string_view(interface) == wl_seat_interface.name) {
      state.seatVersion = std::min(version, 9U);
      state.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, state.seatVersion));
    } else if (std::string_view(interface) == wl_output_interface.name) {
      auto output = std::make_unique<Output>();
      output->version = std::min(version, 4U);
      output->resource =
          static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, output->version));
      wl_output_add_listener(output->resource, &kOutputListener, output.get());
      state.outputs.push_back(std::move(output));
    }
  }

  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener{
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };
} // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argc > 4 || (argc == 4 && std::string_view(argv[3]) != "activate")) {
    std::println(stderr, "usage: foreign-toplevel-client TITLE OUTPUT [activate]");
    return 2;
  }
  const std::string_view wantedTitle = argv[1];
  const std::string_view wantedOutput = argv[2];
  const bool activate = argc == 4;

  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "foreign-toplevel-client: cannot connect to WAYLAND_DISPLAY");
    return 2;
  }

  State state;
  state.registry = wl_display_get_registry(display);
  wl_registry_add_listener(state.registry, &kRegistryListener, &state);

  bool roundtripOk = wl_display_roundtrip(display) >= 0;
  roundtripOk = roundtripOk && wl_display_roundtrip(display) >= 0;
  if (roundtripOk && state.managerName != 0) {
    state.manager = static_cast<zwlr_foreign_toplevel_manager_v1*>(wl_registry_bind(
        state.registry, state.managerName, &zwlr_foreign_toplevel_manager_v1_interface,
        std::min(state.managerVersion, 3U)
    ));
    zwlr_foreign_toplevel_manager_v1_add_listener(state.manager, &kManagerListener, &state);
    roundtripOk = wl_display_roundtrip(display) >= 0;
  }

  const auto output = std::ranges::find_if(state.outputs, [wantedOutput](const auto& candidate) {
    return candidate->name == wantedOutput;
  });
  const bool titleFound = std::ranges::any_of(state.toplevels, [wantedTitle](const auto& toplevel) {
    return !toplevel->closed && toplevel->title == wantedTitle;
  });
  const bool membershipFound =
      output != state.outputs.end() && std::ranges::any_of(state.toplevels, [&](const auto& toplevel) {
        return !toplevel->closed
            && toplevel->title == wantedTitle
            && std::ranges::find(toplevel->outputs, (*output)->resource) != toplevel->outputs.end();
      });

  if (activate && state.seat != nullptr) {
    const auto target = std::ranges::find_if(state.toplevels, [wantedTitle](const auto& toplevel) {
      return !toplevel->closed && toplevel->title == wantedTitle;
    });
    if (target != state.toplevels.end()) {
      zwlr_foreign_toplevel_handle_v1_activate((*target)->handle, state.seat);
      roundtripOk = roundtripOk && wl_display_roundtrip(display) >= 0;
    }
  }

  for (const auto& toplevel : state.toplevels) {
    zwlr_foreign_toplevel_handle_v1_destroy(toplevel->handle);
  }
  if (state.manager != nullptr) {
    zwlr_foreign_toplevel_manager_v1_destroy(state.manager);
  }
  for (const auto& knownOutput : state.outputs) {
    if (knownOutput->version >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
      wl_output_release(knownOutput->resource);
    } else {
      wl_output_destroy(knownOutput->resource);
    }
  }
  if (state.seat != nullptr) {
    if (state.seatVersion >= WL_SEAT_RELEASE_SINCE_VERSION) {
      wl_seat_release(state.seat);
    } else {
      wl_seat_destroy(state.seat);
    }
  }
  wl_registry_destroy(state.registry);
  wl_display_disconnect(display);

  if (!roundtripOk
      || state.manager == nullptr
      || state.finished
      || output == state.outputs.end()
      || (activate && state.seat == nullptr)) {
    std::println(stderr, "foreign-toplevel-client: required protocol state was unavailable");
    return 2;
  }
  if (!membershipFound) {
    std::println(
        stderr, "foreign-toplevel-client: '{}' {} output '{}'", wantedTitle,
        titleFound ? "did not advertise" : "was not advertised on", wantedOutput
    );
    return 1;
  }
  return 0;
}
