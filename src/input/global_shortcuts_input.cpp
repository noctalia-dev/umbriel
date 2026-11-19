#include "input/global_shortcuts_input.h"

#include <algorithm>
#include <utility>

namespace umbriel {

  GlobalShortcutsInput::GlobalShortcutsInput(const GlobalShortcutsRegistry& registry, Event event)
      : m_registry(registry), m_event(std::move(event)) {}

  void GlobalShortcutsInput::retire(Press& press, uint32_t timeMsec) {
    if (const auto active = std::exchange(press.active, std::nullopt); active && m_event) {
      m_event(*active, false, timeMsec);
    }
  }

  bool GlobalShortcutsInput::observeKey(const void* source, uint32_t keycode, uint32_t timeMsec, bool pressed) {
    const auto found = std::ranges::find_if(m_presses, [source, keycode](const auto& press) {
      return press.source == source && press.keycode == keycode;
    });
    if (pressed) {
      if (found == m_presses.end()) {
        m_presses.push_back({.source = source, .keycode = keycode, .active = std::nullopt});
        return false;
      }
      found->fresh = false;
      return found->consumed;
    }
    if (found == m_presses.end()) {
      return false;
    }
    const bool consumed = found->consumed;
    retire(*found, timeMsec);
    m_presses.erase(found);
    return consumed;
  }

  bool GlobalShortcutsInput::handlePress(
      const void* source, uint32_t keycode, uint32_t timeMsec, std::span<const xkb_keysym_t> keysyms,
      xkb_keysym_t rawKeysym, uint32_t modifiers
  ) {
    const auto found = std::ranges::find_if(m_presses, [source, keycode](const auto& press) {
      return press.source == source && press.keycode == keycode;
    });
    if (found == m_presses.end()) {
      return false;
    }
    if (!std::exchange(found->fresh, false)) {
      return found->consumed;
    }
    const auto binding = m_registry.match(keysyms, rawKeysym, modifiers);
    if (!binding) {
      return false;
    }
    found->consumed = true;
    found->active = binding;
    if (m_event) {
      m_event(*binding, true, timeMsec);
    }
    return true;
  }

  void GlobalShortcutsInput::forgetSource(const void* source, uint32_t timeMsec) {
    for (auto& press : m_presses) {
      if (press.source == source) {
        retire(press, timeMsec);
      }
    }
    std::erase_if(m_presses, [source](const auto& press) { return press.source == source; });
  }

  void GlobalShortcutsInput::suspend(uint32_t timeMsec) {
    for (auto& press : m_presses) {
      press.fresh = false;
      retire(press, timeMsec);
    }
  }

  void GlobalShortcutsInput::reconcile(uint32_t timeMsec) {
    for (auto& press : m_presses) {
      if (press.active && !m_registry.owns(*press.active)) {
        retire(press, timeMsec);
      }
    }
  }

} // namespace umbriel
