#pragma once

#include <cstdint>
#include <xkbcommon/xkbcommon.h>

namespace umbriel {

  // Return the active layout's level-zero symbol. When it is printable
  // non-ASCII, prefer the first printable ASCII symbol for the same key in
  // another configured layout.
  xkb_keysym_t rawShortcutKeysym(xkb_keymap* keymap, xkb_keycode_t keycode, xkb_layout_index_t effectiveLayout);

  // Lock masks do not participate in chord matching. The raw symbol provides
  // the configured-layout fallback alongside the effective, lowercased symbol.
  bool matchesShortcutKeysym(
      uint32_t expectedModifiers, xkb_keysym_t expectedKeysym, uint32_t modifiers, xkb_keysym_t keysym,
      xkb_keysym_t rawKeysym
  );

} // namespace umbriel
