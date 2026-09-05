#include "cli/ipc_client.h"

#include "server/ipc_commands.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <print>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace umbriel {

  namespace {
    std::string resolveSocketPath() {
      if (const char* sock = std::getenv("UMBRIEL_SOCKET"); sock != nullptr && sock[0] != '\0') {
        return sock;
      }
      const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
      const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
      if (runtimeDir != nullptr && runtimeDir[0] != '\0' && waylandDisplay != nullptr && waylandDisplay[0] != '\0') {
        return std::string(runtimeDir) + "/umbriel-" + waylandDisplay + ".sock";
      }
      return {};
    }

    // Shared by the one-shot commands and the event stream. Returns a connected fd, or -1 after reporting why.
    int connectToCompositor() {
      std::string socketPath = resolveSocketPath();
      if (socketPath.empty()) {
        std::println(stderr, "error: cannot find umbriel socket (is the compositor running?)");
        return -1;
      }

      int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        std::println(stderr, "error: failed to create socket");
        return -1;
      }

      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      if (socketPath.size() >= sizeof(addr.sun_path)) {
        std::println(stderr, "error: socket path too long");
        close(fd);
        return -1;
      }
      socketPath.copy(addr.sun_path, socketPath.size());

      if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::println(stderr, "error: cannot connect to umbriel socket (is the compositor running?)");
        close(fd);
        return -1;
      }
      return fd;
    }

    bool sendAll(int fd, std::string_view payload) {
      size_t sent = 0;
      while (sent < payload.size()) {
        const ssize_t written = send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (written <= 0) {
          std::println(stderr, "error: failed to send request");
          return false;
        }
        sent += static_cast<size_t>(written);
      }
      return true;
    }
  } // namespace

  int runIpcCommand(const IpcCommandSpec& spec, std::string_view arg, bool json) {
    const int fd = connectToCompositor();
    if (fd < 0) {
      return EXIT_FAILURE;
    }

    // A one-shot request answers immediately or not at all; the event stream deliberately skips this.
    timeval tv{};
    tv.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Build request.
    nlohmann::json req;
    req["cmd"] = spec.name;
    if (spec.takesArg) {
      req["arg"] = std::string(arg);
    }
    std::string payload = req.dump() + "\n";

    if (!sendAll(fd, payload)) {
      close(fd);
      return EXIT_FAILURE;
    }

    // Receive.
    std::string buf;
    char chunk[4096];
    while (true) {
      ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        break;
      }
      buf.append(chunk, static_cast<size_t>(n));
    }
    close(fd);

    if (buf.empty()) {
      std::println(stderr, "error: malformed response");
      return EXIT_FAILURE;
    }

    // Strip trailing newline.
    if (buf.back() == '\n') {
      buf.pop_back();
    }

    auto resp = nlohmann::json::parse(buf, nullptr, false);
    if (resp.is_discarded()) {
      std::println(stderr, "error: malformed response");
      return EXIT_FAILURE;
    }

    if (resp.contains("err")) {
      std::println(stderr, "error: {}", resp["err"].get<std::string>());
      return EXIT_FAILURE;
    }

    if (!resp.contains("ok")) {
      std::println(stderr, "error: malformed response");
      return EXIT_FAILURE;
    }

    const auto& ok = resp["ok"];

    if (json) {
      std::println("{}", ok.dump());
      return EXIT_SUCCESS;
    }

    if (spec.printHuman != nullptr) {
      spec.printHuman(ok);
    }

    return EXIT_SUCCESS;
  }

  int runIpcSubscribe(const std::vector<std::string>& events) {
    const int fd = connectToCompositor();
    if (fd < 0) {
      return EXIT_FAILURE;
    }

    nlohmann::json req;
    req["cmd"] = "subscribe";
    req["events"] = events;
    if (!sendAll(fd, req.dump() + "\n")) {
      close(fd);
      return EXIT_FAILURE;
    }

    // Deliberately no SO_RCVTIMEO: a stream is idle exactly as often as the compositor is. Lines are relayed
    // unparsed, except the first, which is the rejection when a subscription name is unknown - the compositor
    // answers that and closes, so no event can be mistaken for it.
    std::string buf;
    std::array<char, 4096> chunk{};
    bool sawLine = false;
    while (true) {
      const ssize_t received = recv(fd, chunk.data(), chunk.size(), 0);
      if (received <= 0) {
        break;
      }
      buf.append(chunk.data(), static_cast<size_t>(received));
      for (size_t newline = buf.find('\n'); newline != std::string::npos; newline = buf.find('\n')) {
        const std::string line = buf.substr(0, newline);
        buf.erase(0, newline + 1);
        if (!sawLine) {
          sawLine = true;
          const auto parsed = nlohmann::json::parse(line, nullptr, false);
          if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("err")) {
            std::println(stderr, "error: {}", parsed["err"].get<std::string>());
            close(fd);
            return EXIT_FAILURE;
          }
        }
        std::println("{}", line);
        // Line-buffered by hand: a bar reading this pipe must see an event when it happens, and a closed reader
        // ends the stream instead of filling a buffer nobody drains.
        if (std::fflush(stdout) != 0) {
          close(fd);
          return EXIT_FAILURE;
        }
      }
    }
    close(fd);

    if (!sawLine) {
      std::println(stderr, "error: compositor closed the stream without sending anything");
      return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
  }

} // namespace umbriel
