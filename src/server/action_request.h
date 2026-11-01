#pragma once

#include "config/keybind_parse.h"

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>

namespace umbriel {

  // The IPC form of an action: {"cmd":"action","action":"<name>", ...} with one typed field per argument, as
  // docs/user/ipc.md lists. It carries the same payloads as the word grammar without going through text, so programs
  // never depend on how people type actions.

  // Decodes a request into the action and payload of a Keybind. Unknown, mistyped, missing, or out-of-range fields
  // fail with `error` set.
  [[nodiscard]] std::optional<Keybind> actionFromRequest(const nlohmann::json& request, std::string& error);

  // Encodes a parsed action as a request. Empty optional payload fields are left out.
  [[nodiscard]] nlohmann::json actionRequest(const Keybind& bind);

} // namespace umbriel
