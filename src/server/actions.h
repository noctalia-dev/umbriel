#pragma once

#include "config/keybind_parse.h"

#include <string>

namespace umbriel {

  class Server;

  // One action's behaviour. Handlers that consume continuous input receive its normalized scroll amount;
  // ordinary keybindings and IPC pass zero. Discrete handlers receive the action binding and error destination.
  class ActionHandler {
  public:
    using Discrete = bool (*)(Server&, const Keybind&, std::string*);
    using Continuous = bool (*)(Server&, const Keybind&, std::string*, double);

    constexpr ActionHandler(Discrete handler = nullptr) : m_discrete(handler) {}
    constexpr ActionHandler(Continuous handler) : m_continuous(handler) {}
    constexpr explicit operator bool() const { return m_discrete != nullptr || m_continuous != nullptr; }
    bool operator()(Server& server, const Keybind& bind, std::string* error, double scrollNotches) const {
      return m_continuous != nullptr ? m_continuous(server, bind, error, scrollNotches)
                                     : m_discrete(server, bind, error);
    }

  private:
    Discrete m_discrete = nullptr;
    Continuous m_continuous = nullptr;
  };

  // Null when the action has no implementation. Messages written to `error` are surfaced by IPC `msg`.
  // Keeping the dispatch table separate from actionSpecs() leaves the parser independent of Server.
  const ActionHandler* actionHandlerFor(KeybindAction action);

  // True when every advertised action has exactly one handler and every
  // non-sentinel action is advertised.
  bool actionRegistryComplete();

} // namespace umbriel
