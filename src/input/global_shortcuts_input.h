#pragma once

#include "input/global_shortcuts.h"

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace umbriel {

  class GlobalShortcutsInput {
  public:
    using Event = std::function<void(const GlobalShortcutBinding&, bool, uint32_t)>;

    GlobalShortcutsInput(const GlobalShortcutsRegistry& registry, Event event);
    GlobalShortcutsInput(const GlobalShortcutsInput&) = delete;
    GlobalShortcutsInput& operator=(const GlobalShortcutsInput&) = delete;

    // Observe every key before input-routing guards. Releases retire activations
    // here, even when a later guard consumes the event. The result preserves
    // ownership of a swallowed press through repeats, suspension, and release.
    bool observeKey(const void* source, uint32_t keycode, uint32_t timeMsec, bool pressed);
    // Call during the observed press, after configured bindings and routing gates.
    bool handlePress(
        const void* source, uint32_t keycode, uint32_t timeMsec, std::span<const xkb_keysym_t> keysyms,
        xkb_keysym_t rawKeysym, uint32_t modifiers
    );
    void forgetSource(const void* source, uint32_t timeMsec);
    void suspend(uint32_t timeMsec);
    // Retire changed bindings before publishing replacement snapshots.
    void reconcile(uint32_t timeMsec);

  private:
    struct Press {
      const void* source;
      uint32_t keycode;
      bool fresh = true;
      bool consumed = false;
      std::optional<GlobalShortcutBinding> active;
    };

    void retire(Press& press, uint32_t timeMsec);

    const GlobalShortcutsRegistry& m_registry;
    Event m_event;
    std::vector<Press> m_presses;
  };

} // namespace umbriel
