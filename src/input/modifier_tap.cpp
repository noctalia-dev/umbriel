#include "input/modifier_tap.h"

extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

namespace umbriel {
  bool modifierTapPressMatches(uint32_t modifiers, uint32_t pressedModifier, uint32_t expected) {
    const uint32_t effective = (modifiers | pressedModifier) & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    return effective == expected && pressedModifier == expected;
  }

} // namespace umbriel
