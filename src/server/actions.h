#pragma once

#include "config/keybind_parse.h"

#include <string>

namespace umbriel {

  class Server;

  // One action's behaviour. Returns whether the action was consumed; a message
  // written to `error` (when non-null) is surfaced by the IPC `msg` command.
  using ActionHandlerFn = bool (*)(Server& server, const Keybind& bind, std::string* error);

  // Null when the action has no implementation, which the caller reports rather than silently ignoring. Keeping the
  // dispatch table separate from `actionSpecs()` lets the parser stay free of any dependency on Server.
  ActionHandlerFn actionHandlerFor(KeybindAction action);

  // True when every advertised action has exactly one handler and every
  // non-sentinel action is advertised.
  bool actionRegistryComplete();

} // namespace umbriel
