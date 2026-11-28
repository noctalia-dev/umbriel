#pragma once
#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  struct IpcCommandSpec;

  int runIpcCommand(const IpcCommandSpec& spec, std::span<const std::string> args = {}, bool json = false);

  // Parses `words` as an action, sends it as a structured action request, and prints the reply. Word-grammar errors are
  // reported without contacting the compositor.
  int runIpcAction(std::span<const std::string> words, bool json = false);

  // Holds the connection open and relays every event line to stdout until the compositor closes it. Returns failure
  // when a name in `events` is not a subscribable family.
  int runIpcSubscribe(const std::vector<std::string>& events);

  // One inspect command's `ok` payload, or nullopt when no compositor answers before `deadline`, which bounds connect,
  // send, and reply together. Silent by design: shell completion degrades to the compiled-in vocabulary rather than
  // printing or blocking a keystroke.
  [[nodiscard]] std::optional<nlohmann::json>
  queryIpcJson(std::string_view command, std::chrono::steady_clock::time_point deadline);

} // namespace umbriel
