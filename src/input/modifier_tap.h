#pragma once

#include <cstdint>

namespace umbriel {
  // wlroots emits a key event before applying that key to its modifier state.
  // Include the modifier represented by the current press when matching.
  bool modifierTapPressMatches(uint32_t modifiers, uint32_t pressedModifier, uint32_t expected);

} // namespace umbriel
