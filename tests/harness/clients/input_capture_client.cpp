// Exercises the private bridge and observes real EI receiver events. The
// compositor still requires physical consent; this client cannot approve it.
#include "umbriel-input-capture-v1-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <libei.h>
#include <poll.h>
#include <print>
#include <sstream>
#include <string>
#include <unistd.h>
#include <wayland-client.h>

namespace {
  struct State {
    wl_display* display = nullptr;
    umbriel_input_capture_manager_v1* manager = nullptr;
    umbriel_input_capture_session_v1* session = nullptr;
    ei* receiver = nullptr;
    uint32_t protocolVersion = 1;
    uint32_t serial = 1;
    uint32_t zoneSet = 0;
    uint32_t activation = 0;
    std::string pending;
    std::string application = "capture protocol test";
    std::string applicationInstance = "private-test";
    std::string restoreToken;
    std::string grantToken;
    bool done = false;
  };

  void drainEi(State& state) {
    if (state.receiver == nullptr)
      return;
    ei_dispatch(state.receiver);
    while (ei_event* event = ei_get_event(state.receiver)) {
      switch (ei_event_get_type(event)) {
      case EI_EVENT_CONNECT:
        std::puts("ei-connected");
        break;
      case EI_EVENT_DISCONNECT:
        std::puts("ei-disconnected");
        break;
      case EI_EVENT_SEAT_ADDED:
        ei_seat_bind_capabilities(
            ei_event_get_seat(event), EI_DEVICE_CAP_KEYBOARD, EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_BUTTON,
            EI_DEVICE_CAP_SCROLL, nullptr
        );
        break;
      case EI_EVENT_DEVICE_RESUMED:
        std::puts("ei-device-ready");
        break;
      case EI_EVENT_DEVICE_START_EMULATING:
        std::println("ei-start id={}", ei_event_emulating_get_sequence(event));
        break;
      case EI_EVENT_DEVICE_STOP_EMULATING:
        std::puts("ei-stop");
        break;
      case EI_EVENT_KEYBOARD_KEY:
        std::println(
            "ei-key code={} state={}", ei_event_keyboard_get_key(event),
            ei_event_keyboard_get_key_is_press(event) ? "pressed" : "released"
        );
        break;
      case EI_EVENT_POINTER_MOTION:
        std::println("ei-motion dx={:.3f} dy={:.3f}", ei_event_pointer_get_dx(event), ei_event_pointer_get_dy(event));
        break;
      case EI_EVENT_BUTTON_BUTTON:
        std::println(
            "ei-button code={} state={}", ei_event_button_get_button(event),
            ei_event_button_get_is_press(event) ? "pressed" : "released"
        );
        break;
      case EI_EVENT_SCROLL_DELTA:
        std::println("ei-scroll x={:.3f} y={:.3f}", ei_event_scroll_get_dx(event), ei_event_scroll_get_dy(event));
        break;
      case EI_EVENT_SCROLL_DISCRETE:
        std::println(
            "ei-discrete x={} y={}", ei_event_scroll_get_discrete_dx(event), ei_event_scroll_get_discrete_dy(event)
        );
        break;
      case EI_EVENT_FRAME:
        std::puts("ei-frame");
        break;
      default:
        break;
      }
      ei_event_unref(event);
    }
  }

  void started(void*, umbriel_input_capture_session_v1*, uint32_t request, uint32_t status, uint32_t capabilities) {
    std::println("started request={} status={} capabilities={}", request, status, capabilities);
  }
  void zone(
      void*, umbriel_input_capture_session_v1*, uint32_t request, uint32_t width, uint32_t height, int32_t x, int32_t y
  ) {
    std::println("zone request={} x={} y={} width={} height={}", request, x, y, width, height);
  }
  void zonesDone(void* data, umbriel_input_capture_session_v1*, uint32_t request, uint32_t status, uint32_t zoneSet) {
    static_cast<State*>(data)->zoneSet = zoneSet;
    std::println("zones request={} status={} zone_set={}", request, status, zoneSet);
  }
  void barrierFailed(void*, umbriel_input_capture_session_v1*, uint32_t request, uint32_t id) {
    std::println("barrier-failed request={} id={}", request, id);
  }
  void barriersDone(void*, umbriel_input_capture_session_v1*, uint32_t request, uint32_t status) {
    std::println("barriers request={} status={}", request, status);
  }
  void eisFd(void* data, umbriel_input_capture_session_v1*, uint32_t request, int32_t fd) {
    auto& state = *static_cast<State*>(data);
    if (state.receiver != nullptr)
      ei_unref(state.receiver);
    state.receiver = ei_new_receiver(nullptr);
    ei_configure_name(state.receiver, "Umbriel capture protocol test");
    if (ei_setup_backend_fd(state.receiver, fd) != 0)
      state.done = true;
    std::println("eis-fd request={}", request);
  }
  void result(void*, umbriel_input_capture_session_v1*, uint32_t request, uint32_t status) {
    std::println("result request={} status={}", request, status);
  }
  void
  activated(void* data, umbriel_input_capture_session_v1*, uint32_t id, wl_fixed_t x, wl_fixed_t y, uint32_t barrier) {
    static_cast<State*>(data)->activation = id;
    std::println(
        "activated id={} barrier={} x={:.3f} y={:.3f}", id, barrier, wl_fixed_to_double(x), wl_fixed_to_double(y)
    );
  }
  void deactivated(void*, umbriel_input_capture_session_v1*, uint32_t id) { std::println("deactivated id={}", id); }
  void disabled(void*, umbriel_input_capture_session_v1*) { std::puts("disabled"); }
  void zonesChanged(void*, umbriel_input_capture_session_v1*, uint32_t serial) {
    std::println("zones-changed invalidated={}", serial);
  }
  void closed(void*, umbriel_input_capture_session_v1*) { std::puts("closed"); }

  void grant(void* data, umbriel_input_capture_session_v1*, uint32_t request, uint32_t mode, const char* token) {
    static_cast<State*>(data)->grantToken = token;
    std::println("grant request={} mode={} token={}", request, mode, *token != '\0' ? 1 : 0);
  }

  constexpr umbriel_input_capture_session_v1_listener kSessionListener{
      .started = started,
      .zone = zone,
      .zones_done = zonesDone,
      .barrier_failed = barrierFailed,
      .barriers_done = barriersDone,
      .eis_fd = eisFd,
      .result = result,
      .activated = activated,
      .deactivated = deactivated,
      .disabled = disabled,
      .zones_changed = zonesChanged,
      .closed = closed,
      .grant = grant,
  };
  void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, umbriel_input_capture_manager_v1_interface.name) == 0) {
      state.manager = static_cast<umbriel_input_capture_manager_v1*>(wl_registry_bind(
          registry, name, &umbriel_input_capture_manager_v1_interface, std::min(version, state.protocolVersion)
      ));
    }
  }
  void globalRemove(void*, wl_registry*, uint32_t) {}
  constexpr wl_registry_listener kRegistryListener{.global = global, .global_remove = globalRemove};

  void command(State& state, const std::string& line) {
    std::istringstream input(line);
    std::string action;
    input >> action;
    const uint32_t request = state.serial++;
    if (action == "start") {
      uint32_t capabilities = 3;
      uint32_t persistMode = 0;
      input >> capabilities >> persistMode;
      if (state.protocolVersion >= 2)
        umbriel_input_capture_session_v1_set_persistence(
            state.session, persistMode, state.restoreToken.c_str(), state.applicationInstance.c_str()
        );
      umbriel_input_capture_session_v1_start(state.session, request, state.application.c_str(), capabilities);
    } else if (action == "application") {
      std::getline(input >> std::ws, state.application);
      std::println("application length={}", state.application.size());
    } else if (action == "scope") {
      input >> state.applicationInstance;
      std::puts("scope-set");
    } else if (action == "expire-scope") {
      std::string scope = state.applicationInstance;
      input >> scope;
      umbriel_input_capture_manager_v1_expire_scope(state.manager, scope.c_str());
      std::puts("scope-expired");
    } else if (action == "load-grant") {
      std::string path;
      input >> path;
      std::ifstream file(path);
      if (!std::getline(file, state.restoreToken) || state.restoreToken.empty()) {
        std::println(stderr, "cannot load grant");
        state.done = true;
      } else {
        std::puts("grant-loaded");
      }
    } else if (action == "save-grant") {
      std::string path;
      input >> path;
      if (path.empty() || state.grantToken.empty()) {
        std::println(stderr, "cannot save empty grant");
        state.done = true;
      } else {
        std::ofstream file(path);
        file << state.grantToken << '\n';
        file.close();
        if (!file) {
          std::println(stderr, "cannot save grant");
          state.done = true;
        } else {
          std::puts("grant-saved");
        }
      }
    } else if (action == "new-session") {
      if (state.receiver != nullptr)
        state.receiver = ei_unref(state.receiver);
      if (state.session != nullptr)
        umbriel_input_capture_session_v1_destroy(state.session);
      state.session = umbriel_input_capture_manager_v1_create_session(state.manager);
      umbriel_input_capture_session_v1_add_listener(state.session, &kSessionListener, &state);
      state.activation = 0;
      state.zoneSet = 0;
      std::puts("ready new-session");
    } else if (action == "zones") {
      umbriel_input_capture_session_v1_get_zones(state.session, request);
    } else if (action == "barrier") {
      uint32_t id = 1;
      int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
      input >> id >> x1 >> y1 >> x2 >> y2;
      uint32_t zoneSet = state.zoneSet;
      input >> zoneSet;
      umbriel_input_capture_session_v1_clear_barriers(state.session);
      umbriel_input_capture_session_v1_add_barrier(state.session, id, x1, y1, x2, y2);
      umbriel_input_capture_session_v1_commit_barriers(state.session, request, zoneSet);
    } else if (action == "connect") {
      umbriel_input_capture_session_v1_connect_eis(state.session, request);
    } else if (action == "enable") {
      umbriel_input_capture_session_v1_enable(state.session, request);
    } else if (action == "disable") {
      umbriel_input_capture_session_v1_disable(state.session, request);
    } else if (action == "release") {
      uint32_t activation = state.activation;
      double x = 400, y = 300;
      input >> activation >> x >> y;
      umbriel_input_capture_session_v1_release(
          state.session, request, activation, 1, wl_fixed_from_double(x), wl_fixed_from_double(y)
      );
    } else if (action == "close") {
      umbriel_input_capture_session_v1_destroy(state.session);
      state.session = nullptr;
    } else if (action == "mark") {
      std::string label;
      input >> label;
      std::println("mark {}", label);
    } else if (action == "quit") {
      state.done = true;
    } else {
      std::println(stderr, "unknown command: {}", line);
      state.done = true;
    }
    wl_display_flush(state.display);
  }
} // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  State state;
  if (const char* version = std::getenv("CAPTURE_PROTOCOL_VERSION"))
    state.protocolVersion = std::strtoul(version, nullptr, 10);
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr)
    return 1;
  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  if (wl_display_roundtrip(state.display) < 0 || state.manager == nullptr)
    return 1;
  state.session = umbriel_input_capture_manager_v1_create_session(state.manager);
  umbriel_input_capture_session_v1_add_listener(state.session, &kSessionListener, &state);
  wl_display_flush(state.display);
  fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
  std::puts("ready");
  while (!state.done) {
    wl_display_dispatch_pending(state.display);
    drainEi(state);
    wl_display_flush(state.display);
    pollfd fds[]{
        {.fd = wl_display_get_fd(state.display), .events = POLLIN, .revents = 0},
        {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
        {.fd = state.receiver != nullptr ? ei_get_fd(state.receiver) : -1, .events = POLLIN, .revents = 0}
    };
    if (poll(fds, 3, 100) < 0 && errno != EINTR)
      break;
    if ((fds[0].revents & (POLLERR | POLLHUP)) != 0)
      break;
    if ((fds[0].revents & POLLIN) != 0 && wl_display_dispatch(state.display) < 0)
      break;
    if ((fds[1].revents & POLLIN) != 0) {
      char bytes[2048];
      const ssize_t count = read(STDIN_FILENO, bytes, sizeof(bytes));
      if (count > 0)
        state.pending.append(bytes, static_cast<size_t>(count));
      else if (count == 0)
        state.done = true;
      size_t newline = 0;
      while ((newline = state.pending.find('\n')) != std::string::npos) {
        const std::string line = state.pending.substr(0, newline);
        state.pending.erase(0, newline + 1);
        command(state, line);
      }
    }
  }
  if (state.receiver != nullptr)
    ei_unref(state.receiver);
  if (state.session != nullptr)
    umbriel_input_capture_session_v1_destroy(state.session);
  umbriel_input_capture_manager_v1_destroy(state.manager);
  wl_registry_destroy(registry);
  wl_display_disconnect(state.display);
  return 0;
}
