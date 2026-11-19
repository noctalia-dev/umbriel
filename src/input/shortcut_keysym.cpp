#include "input/shortcut_keysym.h"

#include <cmath>
extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

#include <cstdint>

namespace umbriel {

  namespace {

    xkb_keysym_t levelZeroKeysym(xkb_keymap* keymap, xkb_keycode_t keycode, xkb_layout_index_t layout) {
      const xkb_keysym_t* keysyms = nullptr;
      const int count = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, 0, &keysyms);
      return count > 0 && keysyms != nullptr ? keysyms[0] : XKB_KEY_NoSymbol;
    }

    bool isPrintableAscii(xkb_keysym_t keysym) {
      const uint32_t codepoint = xkb_keysym_to_utf32(keysym);
      return codepoint >= 0x20 && codepoint <= 0x7e;
    }

  } // namespace

  xkb_keysym_t rawShortcutKeysym(xkb_keymap* keymap, xkb_keycode_t keycode, xkb_layout_index_t effectiveLayout) {
    if (keymap == nullptr || effectiveLayout >= xkb_keymap_num_layouts(keymap)) {
      return XKB_KEY_NoSymbol;
    }

    const xkb_keysym_t active = levelZeroKeysym(keymap, keycode, effectiveLayout);
    const uint32_t activeCodepoint = xkb_keysym_to_utf32(active);
    if (activeCodepoint == 0 || activeCodepoint <= 0x7f) {
      return active;
    }

    const xkb_layout_index_t layoutCount = xkb_keymap_num_layouts(keymap);
    for (xkb_layout_index_t layout = 0; layout < layoutCount; ++layout) {
      if (layout == effectiveLayout) {
        continue;
      }
      const xkb_keysym_t candidate = levelZeroKeysym(keymap, keycode, layout);
      if (isPrintableAscii(candidate)) {
        return candidate;
      }
    }

    return active;
  }

  bool matchesShortcutKeysym(
      uint32_t expectedModifiers, xkb_keysym_t expectedKeysym, uint32_t modifiers, xkb_keysym_t keysym,
      xkb_keysym_t rawKeysym
  ) {
    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    return effective == expectedModifiers
        && (xkb_keysym_to_lower(keysym) == expectedKeysym || rawKeysym == expectedKeysym);
  }

} // namespace umbriel
