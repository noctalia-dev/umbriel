#pragma once

#include <cstdint>
#include <optional>

namespace umbriel {

  // The VT a key press asks for, or nothing when it asks for none.
  //
  // Two spellings name the same VT: the XF86Switch_VT_1..12 keysyms that the
  // keymap puts on Ctrl+Alt+F1..F12 through `srvr_ctrl(fkey2vt)`, and the bare
  // F1..F12 keysyms those same keys also emit. Both have to be answered,
  // because a keymap built without that section produces only the latter.
  std::optional<unsigned> vtFromKeysym(uint32_t keysym, bool ctrlAltHeld);

} // namespace umbriel
