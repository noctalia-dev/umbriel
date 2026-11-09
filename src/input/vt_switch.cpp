#include "input/vt_switch.h"

#include <xkbcommon/xkbcommon.h>

namespace umbriel {

  std::optional<unsigned> vtFromKeysym(uint32_t keysym, bool ctrlAltHeld) {
    if (keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
      return 1U + (keysym - XKB_KEY_XF86Switch_VT_1);
    }
    if (ctrlAltHeld && keysym >= XKB_KEY_F1 && keysym <= XKB_KEY_F12) {
      return 1U + (keysym - XKB_KEY_F1);
    }
    return std::nullopt;
  }

} // namespace umbriel
