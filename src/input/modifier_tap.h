#pragma once

#include "config/keybind_parse.h"

#include <cstdint>
#include <optional>

namespace umbriel {
  // wlroots emits a key event before applying that key to its modifier state.
  // Include the modifier represented by the current press when matching.
  bool modifierTapPressMatches(uint32_t modifiers, uint32_t pressedModifier, uint32_t expected);

  // Tracks one seat-wide modifier-only bind from its press until release. The source identity keeps identical keycodes
  // from different keyboards from completing each other's tap.
  class ModifierTapState {
  public:
    // Every other key press invalidates the pending tap. Returns true when a
    // tap was invalidated, so that same press cannot immediately arm another.
    bool cancelForKeyPress();
    void arm(Keybind bind, const void* source, uint32_t keycode);
    [[nodiscard]] std::optional<Keybind> release(const void* source, uint32_t keycode);
    void cancel();
    bool armed() const { return m_bind.has_value(); }

  private:
    std::optional<Keybind> m_bind;
    const void* m_source = nullptr;
    uint32_t m_keycode = 0;
  };

} // namespace umbriel
