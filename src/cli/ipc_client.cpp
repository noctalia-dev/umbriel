#include "cli/ipc_client.h"

#include "config/keybind_parse.h"
#include "server/action_request.h"
#include "server/ipc_commands.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <print>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
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

    // A reply is one JSON line from a compositor this CLI started talking to; the cap only keeps a peer from growing
    // the buffer without bound while the deadline runs down.
    constexpr size_t kMaxReplySize = 1U << 20;

    // Milliseconds left until `deadline`, rounded up so a sub-millisecond remainder still waits, and 0 once spent.
    int remainingMs(std::chrono::steady_clock::time_point deadline) {
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      return left.count() > 0 ? static_cast<int>(left.count()) : 0;
    }

    // Arms `option` with what `deadline` has left. False once it is spent, because a zero timeval means no timeout at
    // all.
    bool setTimeout(int fd, int option, std::chrono::steady_clock::time_point deadline) {
      const int remaining = remainingMs(deadline);
      if (remaining <= 0) {
        return false;
      }
      // A sub-second budget needs the microsecond field; the second field alone would round it to nothing.
      timeval tv{};
      tv.tv_sec = remaining / 1000;
      tv.tv_usec = (remaining % 1000) * 1000;
      return setsockopt(fd, SOL_SOCKET, option, &tv, sizeof(tv)) == 0;
    }

    // Shared by the one-shot commands and the event stream. Returns a connected fd, or -1 with `error` set. A caller
    // that owes a keystroke a budget passes `deadline` and gets a connect that cannot outlast it.
    int
    connectSocket(std::string& error, std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
      if (deadline && remainingMs(*deadline) <= 0) {
        error = "completion budget spent before the query";
        return -1;
      }
      std::string socketPath = resolveSocketPath();
      if (socketPath.empty()) {
        error = "cannot find umbriel socket (is the compositor running?)";
        return -1;
      }

      const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | (deadline ? SOCK_NONBLOCK : 0), 0);
      if (fd < 0) {
        error = "failed to create socket";
        return -1;
      }

      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      if (socketPath.size() >= sizeof(addr.sun_path)) {
        error = "socket path too long";
        close(fd);
        return -1;
      }
      socketPath.copy(addr.sun_path, socketPath.size());

      // AF_UNIX connects at once or refuses — a listener whose accept queue is full answers `EAGAIN` — so the
      // non-blocking socket only keeps a refusal from waiting. The owner's budget covers the rest of the request.
      if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        error = "cannot connect to umbriel socket (is the compositor running?)";
        close(fd);
        return -1;
      }
      if (deadline) {
        // The query bounds its send and receive with socket timeouts, which a non-blocking fd ignores.
        const int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
          error = "cannot connect to umbriel socket (is the compositor running?)";
          close(fd);
          return -1;
        }
      }
      return fd;
    }

    int connectToCompositor() {
      std::string error;
      const int fd = connectSocket(error);
      if (fd < 0) {
        std::println(stderr, "error: {}", error);
      }
      return fd;
    }

    // One reply line without its terminator, or nullopt when the peer closed without one, the reply exceeded the cap,
    // or the deadline passed. A timeout covers a single `recv`, so a peer that trickles the reply is re-armed against
    // the same deadline instead of extending it. Reading stops at the first newline: a peer that answers and then holds
    // the connection open must not cost the caller the rest of its budget. The caller owns `fd` and closes it.
    std::optional<std::string>
    receiveLine(int fd, std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
      std::string buf;
      char chunk[4096];
      while (true) {
        if (deadline && !setTimeout(fd, SO_RCVTIMEO, *deadline)) {
          return std::nullopt;
        }
        const ssize_t received = recv(fd, chunk, sizeof(chunk), 0);
        if (received <= 0) {
          return std::nullopt;
        }
        buf.append(chunk, static_cast<size_t>(received));
        const size_t newline = buf.find('\n');
        if (newline != std::string::npos) {
          buf.resize(newline);
          return buf;
        }
        if (buf.size() > kMaxReplySize) {
          return std::nullopt;
        }
      }
    }

    bool sendAll(int fd, std::string_view payload, bool report = true) {
      size_t sent = 0;
      while (sent < payload.size()) {
        const ssize_t written = send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (written <= 0) {
          if (report) {
            std::println(stderr, "error: failed to send request");
          }
          return false;
        }
        sent += static_cast<size_t>(written);
      }
      return true;
    }

    std::string requestPayload(nlohmann::json request) {
      if (const char* token = std::getenv("UMBRIEL_LAUNCH_TOKEN"); token != nullptr && token[0] != '\0') {
        request["launch_token"] = token;
      }
      return request.dump() + "\n";
    }

    // Sends one request and prints its reply: the `ok` value as JSON, through `printHuman`, or nothing.
    int
    exchange(const nlohmann::json& request, int replyTimeoutSec, void (*printHuman)(const nlohmann::json&), bool json) {
      const int fd = connectToCompositor();
      if (fd < 0) {
        return EXIT_FAILURE;
      }

      // A one-shot request answers within its command's limit or not at all; the event stream deliberately skips this.
      timeval tv{};
      tv.tv_sec = replyTimeoutSec;
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      if (!sendAll(fd, requestPayload(request))) {
        close(fd);
        return EXIT_FAILURE;
      }

      const std::optional<std::string> line = receiveLine(fd);
      close(fd);
      if (!line) {
        std::println(stderr, "error: malformed response");
        return EXIT_FAILURE;
      }

      auto resp = nlohmann::json::parse(*line, nullptr, false);
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

      if (printHuman != nullptr) {
        printHuman(ok);
      }

      return EXIT_SUCCESS;
    }
  } // namespace

  int runIpcCommand(const IpcCommandSpec& spec, std::span<const std::string> args, bool json) {
    nlohmann::json req;
    req["cmd"] = spec.name;
    if (spec.takesArgs) {
      nlohmann::json array = nlohmann::json::array();
      for (const auto& arg : args) {
        array.push_back(arg);
      }
      req["args"] = std::move(array);
    }
    return exchange(req, spec.replyTimeoutSec, spec.printHuman, json);
  }

  int runIpcAction(std::span<const std::string> words, bool json) {
    const ActionSpec* spec = words.empty() ? nullptr : findActionSpec(words.front());
    if (spec == nullptr) {
      std::println(stderr, "error: unknown action: {}", words.empty() ? std::string{} : words.front());
      return EXIT_FAILURE;
    }
    Keybind bind;
    if (!parseActionWords(words, bind)) {
      std::println(
          stderr, "error: invalid arguments for {}; usage: {}{}{}", spec->name, spec->name,
          spec->param.empty() ? "" : " ", spec->param
      );
      return EXIT_FAILURE;
    }
    return exchange(actionRequest(bind), 2, nullptr, json);
  }

  int runIpcSubscribe(const std::vector<std::string>& events) {
    const int fd = connectToCompositor();
    if (fd < 0) {
      return EXIT_FAILURE;
    }

    nlohmann::json req;
    req["cmd"] = "subscribe";
    req["events"] = events;
    if (!sendAll(fd, requestPayload(std::move(req)))) {
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

  std::optional<nlohmann::json> queryIpcJson(std::string_view command, std::chrono::steady_clock::time_point deadline) {
    std::string error;
    const int fd = connectSocket(error, deadline);
    if (fd < 0) {
      return std::nullopt;
    }

    nlohmann::json request;
    request["cmd"] = command;
    const bool sent = setTimeout(fd, SO_SNDTIMEO, deadline) && sendAll(fd, request.dump() + "\n", false);
    const std::optional<std::string> line = sent ? receiveLine(fd, deadline) : std::nullopt;
    close(fd);
    if (!line) {
      return std::nullopt;
    }
    const nlohmann::json resp = nlohmann::json::parse(*line, nullptr, false);
    if (resp.is_discarded() || !resp.is_object() || !resp.contains("ok")) {
      return std::nullopt;
    }
    // nlohmann 3.12 answers both optional<json>'s own converting constructor and json's template conversion
    // operator, so the implicit conversion is ambiguous; in_place names the one constructor meant.
    return std::optional<nlohmann::json>{std::in_place, resp["ok"]};
  }

} // namespace umbriel
