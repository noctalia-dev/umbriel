#include "server/ipc.h"

#include "config/config.h"
#include "core/log.h"
#include "overview/overview.h"
#include "scene/color.h"
#include "server/ipc_commands.h"
#include "server/server.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <wayland-server-core.h>

namespace umbriel {

  namespace {
    constexpr Logger kLog("ipc");
    constexpr size_t kMaxRequestSize = 65536;
    // A subscriber that stops reading must not grow the compositor's heap without bound.
    constexpr size_t kMaxOutboundBacklog = 256 * 1024;
    constexpr int kConnectionTimeoutMs = 1000;

    nlohmann::json themeEvent() {
      const auto& current = config();
      const auto& colors = current.colors;
      return nlohmann::json{
          {"event", "theme"},
          {"data",
           {
               {"background", rgbaHex(colors.background)},
               {"text_primary", rgbaHex(colors.textPrimary)},
               {"text_muted", rgbaHex(colors.textMuted)},
               {"accent_primary", rgbaHex(colors.accentPrimary)},
               {"accent_secondary", rgbaHex(colors.accentSecondary)},
               {"warning", rgbaHex(colors.warning)},
               {"error", rgbaHex(colors.error)},
               {"corner_radius", current.appearance.cornerRadius},
           }},
      };
    }

    nlohmann::json overviewEvent(Server& server) {
      const Overview* overview = server.overview();
      return nlohmann::json{
          {"event", "overview"},
          {"data", {{"open", overview != nullptr && overview->active()}}},
      };
    }

    // Nullopt when no physical keyboard exists; the caller then skips the send
    // entirely so the event stream simply starts when a keyboard arrives.
    std::optional<nlohmann::json> keyboardLayoutEvent(Server& server) {
      const auto state = server.keyboardLayoutState();
      if (!state.has_value()) {
        return std::nullopt;
      }
      return nlohmann::json{
          {"event", "keyboard_layout"},
          {"data", {{"names", state->names}, {"current_index", state->currentIndex}}},
      };
    }

    nlohmann::json windowsEvent(Server& server) {
      // Reuses the command handler so the event payload can never diverge from
      // what `umbriel windows` reports.
      return nlohmann::json{{"event", "windows"}, {"data", IpcCommands::windows(server, {}).at("ok")}};
    }
  } // namespace

  Ipc::Ipc(Server& server, const std::string& waylandSocketName) : m_server(&server) {
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr || runtimeDir[0] == '\0') {
      kLog.error("XDG_RUNTIME_DIR not set, IPC socket disabled");
      return;
    }
    m_socketPath = std::string(runtimeDir) + "/umbriel-" + waylandSocketName + ".sock";

    m_listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (m_listenFd < 0) {
      kLog.error("failed to create IPC socket: {}", strerror(errno));
      return;
    }

    unlink(m_socketPath.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_socketPath.size() >= sizeof(addr.sun_path)) {
      kLog.error("IPC socket path too long");
      close(m_listenFd);
      m_listenFd = -1;
      return;
    }
    m_socketPath.copy(addr.sun_path, m_socketPath.size());

    if (bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      kLog.error("failed to bind IPC socket: {}", strerror(errno));
      close(m_listenFd);
      m_listenFd = -1;
      return;
    }

    if (listen(m_listenFd, 4) < 0) {
      kLog.error("failed to listen on IPC socket: {}", strerror(errno));
      close(m_listenFd);
      m_listenFd = -1;
      unlink(m_socketPath.c_str());
      return;
    }

    m_eventSource = wl_event_loop_add_fd(
        wl_display_get_event_loop(server.display()), m_listenFd, WL_EVENT_READABLE, onListenReadable, this
    );
    if (m_eventSource == nullptr) {
      kLog.error("failed to register IPC event source");
      close(m_listenFd);
      m_listenFd = -1;
      unlink(m_socketPath.c_str());
      return;
    }

    kLog.info("IPC listening on {}", m_socketPath);
  }

  Ipc::~Ipc() {
    if (m_eventSource != nullptr) {
      wl_event_source_remove(m_eventSource);
      m_eventSource = nullptr;
    }
    if (m_listenFd >= 0) {
      close(m_listenFd);
      m_listenFd = -1;
    }
    for (const auto& connection : m_connections) {
      closeConnection(*connection);
    }
    m_connections.clear();
    if (!m_socketPath.empty()) {
      unlink(m_socketPath.c_str());
    }
  }

  int Ipc::onListenReadable(int /*fd*/, uint32_t /*mask*/, void* data) {
    static_cast<Ipc*>(data)->acceptConnections();
    return 0;
  }

  void Ipc::acceptConnections() {
    while (true) {
      const int clientFd = accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (clientFd >= 0) {
        addConnection(clientFd);
        continue;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        kLog.warn("failed to accept IPC connection: {}", strerror(errno));
      }
      return;
    }
  }

  void Ipc::addConnection(int clientFd) {
    auto connection = std::make_unique<Connection>();
    connection->owner = this;
    connection->fd = clientFd;

    wl_event_loop* loop = wl_display_get_event_loop(m_server->display());
    connection->fdSource = wl_event_loop_add_fd(loop, clientFd, WL_EVENT_READABLE, onConnectionEvent, connection.get());
    connection->deadline = wl_event_loop_add_timer(loop, onConnectionTimeout, connection.get());
    if (connection->fdSource == nullptr
        || connection->deadline == nullptr
        || wl_event_source_timer_update(connection->deadline, kConnectionTimeoutMs) < 0) {
      kLog.warn("failed to register IPC connection");
      closeConnection(*connection);
      return;
    }
    m_connections.push_back(std::move(connection));
  }

  int Ipc::onConnectionEvent(int /*fd*/, uint32_t mask, void* data) {
    auto* connection = static_cast<Connection*>(data);
    Ipc* owner = connection->owner;
    if ((mask & WL_EVENT_ERROR) != 0) {
      owner->removeConnection(connection);
      return 0;
    }

    bool keep = true;
    if (!connection->responding && (mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP)) != 0) {
      keep = owner->readRequest(*connection);
    }
    if (keep && connection->responding) {
      keep = owner->writeResponse(*connection);
    }
    if (!keep) {
      owner->removeConnection(connection);
    }
    return 0;
  }

  bool Ipc::readRequest(Connection& connection) {
    char chunk[4096];
    while (true) {
      const ssize_t size = recv(connection.fd, chunk, sizeof(chunk), 0);
      if (size > 0) {
        connection.input.append(chunk, static_cast<size_t>(size));
        if (connection.input.size() > kMaxRequestSize) {
          prepareResponse(connection, R"({"err":"request too long"})");
          return true;
        }
        if (connection.input.contains('\n')) {
          const size_t newline = connection.input.find('\n');
          const std::string line = connection.input.substr(0, newline);
          connection.input.erase(0, newline + 1);
          prepareResponse(connection, handleRequest(connection, line));
          return true;
        }
        continue;
      }
      if (size == 0) {
        if (connection.input.empty()) {
          return false;
        }
        const std::string line = std::move(connection.input);
        connection.input.clear();
        prepareResponse(connection, handleRequest(connection, line));
        return true;
      }
      if (errno == EINTR) {
        continue;
      }
      return errno == EAGAIN || errno == EWOULDBLOCK;
    }
  }

  void Ipc::prepareResponse(Connection& connection, std::string response) {
    connection.output = std::move(response);
    connection.output += '\n';
    connection.responding = true;
  }

  bool Ipc::writeResponse(Connection& connection) {
    while (connection.writeOffset < connection.output.size()) {
      const ssize_t size = send(
          connection.fd, connection.output.data() + connection.writeOffset,
          connection.output.size() - connection.writeOffset, MSG_NOSIGNAL
      );
      if (size > 0) {
        connection.writeOffset += static_cast<size_t>(size);
        continue;
      }
      if (size < 0 && errno == EINTR) {
        continue;
      }
      if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return wl_event_source_fd_update(connection.fdSource, WL_EVENT_WRITABLE) >= 0;
      }
      return false;
    }
    if (connection.subscribedEvents != 0) {
      connection.output.clear();
      connection.writeOffset = 0;
      connection.responding = false;
      return wl_event_source_fd_update(connection.fdSource, WL_EVENT_READABLE) >= 0;
    }
    return false;
  }

  int Ipc::onConnectionTimeout(void* data) {
    auto* connection = static_cast<Connection*>(data);
    connection->owner->removeConnection(connection);
    return 0;
  }

  void Ipc::removeConnection(Connection* connection) {
    const auto entry = std::ranges::find_if(m_connections, [connection](const auto& candidate) {
      return candidate.get() == connection;
    });
    if (entry == m_connections.end()) {
      return;
    }
    closeConnection(**entry);
    m_connections.erase(entry);
  }

  void Ipc::closeConnection(Connection& connection) {
    if (connection.fdSource != nullptr) {
      wl_event_source_remove(connection.fdSource);
      connection.fdSource = nullptr;
    }
    if (connection.deadline != nullptr) {
      wl_event_source_remove(connection.deadline);
      connection.deadline = nullptr;
    }
    if (connection.fd >= 0) {
      close(connection.fd);
      connection.fd = -1;
    }
  }

  std::string Ipc::handleRequest(Connection& connection, std::string_view line) {
    auto req = nlohmann::json::parse(line, nullptr, false);
    if (req.is_discarded() || !req.is_object() || !req.contains("cmd") || !req["cmd"].is_string()) {
      return R"({"err":"malformed request"})";
    }
    const std::string cmd = req["cmd"].get<std::string>();
    if (cmd == "subscribe") {
      if (!req.contains("events") || !req["events"].is_array() || req["events"].empty()) {
        return R"({"err":"malformed request"})";
      }
      uint8_t requested = 0;
      for (const auto& event : req["events"]) {
        if (!event.is_string()) {
          return R"({"err":"malformed request"})";
        }
        const auto& name = event.get_ref<const std::string&>();
        if (name == "theme") {
          requested |= Ipc::kEventTheme;
        } else if (name == "overview") {
          requested |= Ipc::kEventOverview;
        } else if (name == "keyboard_layout") {
          requested |= Ipc::kEventKeyboardLayout;
        } else if (name == "windows") {
          requested |= Ipc::kEventWindows;
        } else {
          return nlohmann::json{{"err", "unknown subscription event: " + name}}.dump();
        }
      }
      connection.subscribedEvents |= requested;
      if (connection.deadline != nullptr) {
        wl_event_source_remove(connection.deadline);
        connection.deadline = nullptr;
      }
      // Initial-state lines in a fixed order; prepareResponse appends the final newline. A keyboard_layout line with no
      // keyboard is skipped, so that stream simply starts when a keyboard arrives.
      std::string response;
      auto append = [&response](const nlohmann::json& event) {
        if (!response.empty()) {
          response += '\n';
        }
        response += event.dump();
      };
      if ((requested & Ipc::kEventTheme) != 0) {
        append(themeEvent());
      }
      if ((requested & Ipc::kEventOverview) != 0) {
        append(overviewEvent(*m_server));
      }
      if ((requested & Ipc::kEventKeyboardLayout) != 0) {
        if (const auto event = keyboardLayoutEvent(*m_server)) {
          append(*event);
        }
      }
      if ((requested & Ipc::kEventWindows) != 0) {
        append(windowsEvent(*m_server));
      }
      return response;
    }
    const IpcCommandSpec* spec = findIpcCommand(cmd);
    if (spec == nullptr) {
      return nlohmann::json{{"err", "unknown command: " + cmd}}.dump();
    }
    std::string arg;
    if (spec->takesArg) {
      if (!req.contains("arg") || !req["arg"].is_string()) {
        return R"({"err":"malformed request"})";
      }
      arg = req["arg"].get<std::string>();
    }
    return spec->handle(*m_server, arg).dump();
  }

  void Ipc::broadcastEvent(uint8_t event, const nlohmann::json& payload) {
    const std::string update = payload.dump() + '\n';
    std::vector<Connection*> evicted;
    for (const auto& connection : m_connections) {
      if ((connection->subscribedEvents & event) == 0) {
        continue;
      }
      if (connection->responding) {
        // output keeps everything written so far; only the undrained tail counts against the cap.
        const size_t backlog = connection->output.size() - connection->writeOffset;
        if (backlog + update.size() > kMaxOutboundBacklog) {
          kLog.warn("subscriber fd {} is not draining ({} bytes queued), disconnecting", connection->fd, backlog);
          evicted.push_back(connection.get());
          continue;
        }
        connection->output += update;
      } else {
        connection->output = update;
        connection->writeOffset = 0;
        connection->responding = true;
      }
      wl_event_source_fd_update(connection->fdSource, WL_EVENT_READABLE | WL_EVENT_WRITABLE);
    }
    for (Connection* connection : evicted) {
      removeConnection(connection);
    }
  }

  void Ipc::notifyThemeChanged() { broadcastEvent(kEventTheme, themeEvent()); }

  void Ipc::notifyOverviewChanged() { broadcastEvent(kEventOverview, overviewEvent(*m_server)); }

  void Ipc::notifyKeyboardLayoutChanged() {
    if (const auto event = keyboardLayoutEvent(*m_server)) {
      broadcastEvent(kEventKeyboardLayout, *event);
    }
  }

  void Ipc::notifyWindowsChanged() { broadcastEvent(kEventWindows, windowsEvent(*m_server)); }

} // namespace umbriel
