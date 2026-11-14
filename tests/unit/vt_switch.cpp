#include "input/vt_switch.h"

#include "check.h"

#include <optional>
#include <xkbcommon/xkbcommon.h>

UMBRIEL_TEST(vtKeysymsNameTheirVt) {
  CHECK_EQ(umbriel::vtFromKeysym(XKB_KEY_XF86Switch_VT_1, false), std::optional<unsigned>(1U));
  CHECK_EQ(umbriel::vtFromKeysym(XKB_KEY_XF86Switch_VT_2, false), std::optional<unsigned>(2U));
  CHECK_EQ(umbriel::vtFromKeysym(XKB_KEY_XF86Switch_VT_12, false), std::optional<unsigned>(12U));
}

// The keymap puts XF86Switch_VT_n on Ctrl+Alt+F<n>, so the modifier state is not
// part of that spelling: the keysym alone decides, and requiring Ctrl+Alt would
// drop the press on a keymap that maps the keysym onto a bare key.
UMBRIEL_TEST(vtKeysymsDoNotNeedCtrlAlt) {
  CHECK_EQ(umbriel::vtFromKeysym(XKB_KEY_XF86Switch_VT_1, true), std::optional<unsigned>(1U));
}

// A keymap built without `srvr_ctrl(fkey2vt)` leaves bare F1..F12 on those keys,
// which only means "switch VT" while Ctrl and Alt are held.
UMBRIEL_TEST(bareFunctionKeysNeedCtrlAlt) {
  CHECK_EQ(umbriel::vtFromKeysym(XKB_KEY_F1, true), std::optional<unsigned>(1U));
  CHECK_EQ(umbriel::vtFromKeysym(XKB_KEY_F5, true), std::optional<unsigned>(5U));
  CHECK_EQ(umbriel::vtFromKeysym(XKB_KEY_F12, true), std::optional<unsigned>(12U));
  CHECK(!umbriel::vtFromKeysym(XKB_KEY_F1, false).has_value());
  CHECK(!umbriel::vtFromKeysym(XKB_KEY_F12, false).has_value());
}

// F13..F24 are what a keyboard with extra function keys sends. F13 must not read
// as VT13, and nothing outside F1..F12 may be claimed.
UMBRIEL_TEST(functionKeysPastTwelveAreNotVts) {
  CHECK(!umbriel::vtFromKeysym(XKB_KEY_F13, true).has_value());
  CHECK(!umbriel::vtFromKeysym(XKB_KEY_F24, true).has_value());
}

UMBRIEL_TEST(ordinaryKeysAreNotVts) {
  CHECK(!umbriel::vtFromKeysym(XKB_KEY_q, true).has_value());
  CHECK(!umbriel::vtFromKeysym(XKB_KEY_Escape, true).has_value());
  CHECK(!umbriel::vtFromKeysym(XKB_KEY_Return, true).has_value());
  // Another XF86 keysym in the same family is not a VT switch either.
  CHECK(!umbriel::vtFromKeysym(XKB_KEY_XF86AudioMute, false).has_value());
}

int main() { return RUN_TESTS(); }
