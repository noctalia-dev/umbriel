#pragma once

#include <xkbcommon/xkbcommon.h>

namespace umbriel {

  // Return the active layout's level-zero symbol. When it is printable
  // non-ASCII, prefer the first printable ASCII symbol for the same key in
  // another configured layout.
  xkb_keysym_t rawShortcutKeysym(xkb_keymap* keymap, xkb_keycode_t keycode, xkb_layout_index_t effectiveLayout);

} // namespace umbriel
