#include "core/private_state_file.h"
#include "input/global_shortcuts_store.h"
#include "input/input_capture_permissions.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <poll.h>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <umbriel-global-shortcuts-v1-client-protocol.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>

namespace {
  struct State;
  struct Session {
    State& owner;
    size_t number;
    umbriel_global_shortcuts_session_v1* proxy;
    std::string tokenFile;
    uint32_t bindRequest = 0;
    Session(State& owner, size_t number, umbriel_global_shortcuts_session_v1* proxy)
        : owner(owner), number(number), proxy(proxy) {}
    ~Session() {
      if (proxy != nullptr) {
        umbriel_global_shortcuts_session_v1_destroy(proxy);
      }
    }
  };

  struct State {
    wl_display* display = nullptr;
    umbriel_global_shortcuts_manager_v1* manager = nullptr;
    std::vector<std::unique_ptr<Session>> sessions;
    size_t selected = 0;
    uint32_t serial = 0;
    bool done = false;
    bool failed = false;
  };

  Session& session(void* data) { return *static_cast<Session*>(data); }

  constexpr umbriel_global_shortcuts_session_v1_listener kSessionListener{
      .grant =
          [](void* data, umbriel_global_shortcuts_session_v1*, uint32_t request, const char* token) {
            auto& state = session(data);
            if (state.tokenFile.empty()
                || !umbriel::writePrivateStateFile(state.tokenFile, token, ".shortcut-token-")) {
              state.owner.failed = state.owner.done = true;
            }
            std::println("grant session={} request={}", state.number, request);
          },
      .restored =
          [](void* data, umbriel_global_shortcuts_session_v1*, uint32_t request, uint32_t status) {
            std::println("restored session={} request={} status={}", session(data).number, request, status);
          },
      .shortcut =
          [](void* data, umbriel_global_shortcuts_session_v1*, uint32_t request, const char* id, const char*,
             const char* trigger) {
            std::println("shortcut session={} request={} id={} trigger={}", session(data).number, request, id, trigger);
          },
      .bound = [](
                   void* data, umbriel_global_shortcuts_session_v1*, uint32_t request, uint32_t status
               ) { std::println("bound session={} request={} status={}", session(data).number, request, status); },
      .listed = [](
                    void* data, umbriel_global_shortcuts_session_v1*, uint32_t request, uint32_t status
                ) { std::println("listed session={} request={} status={}", session(data).number, request, status); },
      .activated = [](
                       void* data, umbriel_global_shortcuts_session_v1*, const char* id, uint32_t time
                   ) { std::println("activated session={} id={} time={}", session(data).number, id, time); },
      .deactivated = [](
                         void* data, umbriel_global_shortcuts_session_v1*, const char* id, uint32_t time
                     ) { std::println("deactivated session={} id={} time={}", session(data).number, id, time); },
      .changed = [](
                     void* data, umbriel_global_shortcuts_session_v1*, const char* id, const char*, const char* trigger
                 ) { std::println("changed session={} id={} trigger={}", session(data).number, id, trigger); },
      .changes_done =
          [](void* data, umbriel_global_shortcuts_session_v1*) {
            std::println("changes-done session={}", session(data).number);
          },
      .closed = [](void* data,
                   umbriel_global_shortcuts_session_v1*) { std::println("closed session={}", session(data).number); },
  };

  void command(State& state, const std::string& line) {
    std::istringstream words(line);
    std::string action;
    words >> action;
    if (action == "new") {
      std::string application;
      words >> application;
      auto next = std::make_unique<Session>(
          state, state.sessions.size(),
          umbriel_global_shortcuts_manager_v1_create_session(state.manager, application.c_str())
      );
      umbriel_global_shortcuts_session_v1_add_listener(next->proxy, &kSessionListener, next.get());
      state.selected = next->number;
      state.sessions.push_back(std::move(next));
    } else if (action == "select") {
      if (!(words >> state.selected) || state.selected >= state.sessions.size()) {
        throw std::runtime_error("invalid shortcut session selection");
      }
    } else if (action == "sync") {
      std::string marker;
      words >> marker;
      if (wl_display_roundtrip(state.display) < 0) {
        throw std::runtime_error("shortcut bridge roundtrip failed");
      }
      std::println("sync {}", marker);
    } else if (action == "quit") {
      state.done = true;
    } else {
      auto& current = *state.sessions.at(state.selected);
      if (current.proxy == nullptr) {
        throw std::runtime_error("shortcut session was destroyed");
      }
      const auto request = ++state.serial;
      if (action == "restore") {
        words >> current.tokenFile;
        std::ifstream file(current.tokenFile);
        std::string token;
        if (!std::getline(file, token) || token.empty()) {
          throw std::runtime_error("cannot load shortcut fixture credential");
        }
        umbriel_global_shortcuts_session_v1_restore(current.proxy, request, token.c_str());
      } else if (action == "credential") {
        words >> current.tokenFile;
      } else if (action == "add") {
        std::string id;
        std::string trigger;
        words >> id >> trigger;
        std::string description;
        std::getline(words >> std::ws, description);
        umbriel_global_shortcuts_session_v1_add_shortcut(
            current.proxy, id.c_str(), description.empty() ? "Test action" : description.c_str(),
            trigger == "-" ? "" : trigger.c_str()
        );
      } else if (action == "add-many") {
        size_t count = 0;
        size_t length = 0;
        if (!(words >> count >> length) || count > umbriel::kMaxGlobalShortcuts || length > 1024) {
          throw std::runtime_error("invalid shortcut batch fixture");
        }
        const std::string description(length, 'x');
        for (size_t index = 0; index < count; ++index) {
          const auto id = std::format("review-{:03}", index);
          umbriel_global_shortcuts_session_v1_add_shortcut(current.proxy, id.c_str(), description.c_str(), "F16");
        }
      } else if (action == "bind") {
        current.bindRequest = request;
        umbriel_global_shortcuts_session_v1_bind(current.proxy, request);
      } else if (action == "cancel") {
        umbriel_global_shortcuts_session_v1_cancel(current.proxy, current.bindRequest);
      } else if (action == "list") {
        umbriel_global_shortcuts_session_v1_list(current.proxy, request);
      } else if (action == "close") {
        umbriel_global_shortcuts_session_v1_destroy(current.proxy);
        current.proxy = nullptr;
      } else {
        throw std::runtime_error("unknown shortcut client command");
      }
    }
    wl_display_flush(state.display);
  }

  int seed(int argc, char** argv) {
    if (argc < 6 || (argc - 4) % 2 != 0) {
      throw std::runtime_error("seed requires TOKEN_FILE APPLICATION ID TRIGGER pairs");
    }
    std::vector<umbriel::GlobalShortcut> shortcuts;
    for (int index = 4; index < argc; index += 2) {
      const auto trigger =
          std::strcmp(argv[index + 1], "-") == 0 ? std::nullopt : umbriel::parseGlobalShortcutTrigger(argv[index + 1]);
      if (!trigger && std::strcmp(argv[index + 1], "-") != 0) {
        throw std::runtime_error("invalid fixture trigger");
      }
      shortcuts.push_back({argv[index], "Test action", trigger});
    }
    umbriel::GlobalShortcutsStore store(umbriel::GlobalShortcutsStore::defaultPath());
    const auto credential = store.approve(argv[3], shortcuts);
    return credential && umbriel::writePrivateStateFile(argv[2], credential->token, ".shortcut-token-") ? 0 : 1;
  }
} // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  try {
    if (argc > 1 && std::strcmp(argv[1], "seed") == 0) {
      return seed(argc, argv);
    }
    if (argc == 5 && std::strcmp(argv[1], "seed-capture") == 0) {
      umbriel::InputCapturePermissions store(umbriel::InputCapturePermissions::defaultPath());
      const auto grant = store.issue(argv[3], static_cast<uint32_t>(std::stoul(argv[4])), 2, 1);
      return grant && umbriel::writePrivateStateFile(argv[2], grant->token, ".capture-token-") ? 0 : 1;
    }
    State state;
    state.display = wl_display_connect(nullptr);
    if (state.display == nullptr) {
      return 1;
    }
    wl_registry* registry = wl_display_get_registry(state.display);
    constexpr wl_registry_listener listener{
        .global =
            [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
              auto& state = *static_cast<State*>(data);
              if (std::strcmp(interface, umbriel_global_shortcuts_manager_v1_interface.name) == 0) {
                state.manager = static_cast<umbriel_global_shortcuts_manager_v1*>(wl_registry_bind(
                    registry, name, &umbriel_global_shortcuts_manager_v1_interface, std::min(version, 1U)
                ));
              }
            },
        .global_remove = [](void*, wl_registry*, uint32_t) {},
    };
    wl_registry_add_listener(registry, &listener, &state);
    if (wl_display_roundtrip(state.display) < 0 || state.manager == nullptr) {
      return 1;
    }
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) != 0) {
      return 1;
    }
    std::puts("ready");
    std::string pending;
    while (!state.done) {
      wl_display_dispatch_pending(state.display);
      wl_display_flush(state.display);
      std::array<pollfd, 2> fds{{
          {.fd = wl_display_get_fd(state.display), .events = POLLIN, .revents = 0},
          {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
      }};
      if (poll(fds.data(), fds.size(), -1) < 0) {
        if (errno == EINTR) {
          continue;
        }
        return 1;
      }
      if ((fds[0].revents & (POLLERR | POLLHUP)) != 0) {
        break;
      }
      if ((fds[0].revents & POLLIN) != 0 && wl_display_dispatch(state.display) < 0) {
        return 1;
      }
      if ((fds[1].revents & (POLLIN | POLLHUP)) != 0) {
        std::array<char, 4096> bytes{};
        const auto count = read(STDIN_FILENO, bytes.data(), bytes.size());
        if (count == 0) {
          break;
        }
        if (count > 0) {
          pending.append(bytes.data(), static_cast<size_t>(count));
          size_t newline = 0;
          while ((newline = pending.find('\n')) != std::string::npos) {
            const auto line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            command(state, line);
          }
        }
      }
    }
    state.sessions.clear();
    umbriel_global_shortcuts_manager_v1_destroy(state.manager);
    wl_registry_destroy(registry);
    wl_display_disconnect(state.display);
    return state.failed ? 1 : 0;
  } catch (const std::exception& error) {
    std::println(stderr, "global-shortcuts-client: {}", error.what());
    return 1;
  }
}
