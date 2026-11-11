#include "input/modifier_tap.h"

#include "check.h"

extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

using umbriel::modifierTapPressMatches;

UMBRIEL_TEST(currentPressContributesModifierBeforeWlrootsStateUpdate) {
  CHECK(modifierTapPressMatches(0, WLR_MODIFIER_LOGO, WLR_MODIFIER_LOGO));
  CHECK(modifierTapPressMatches(WLR_MODIFIER_CAPS, WLR_MODIFIER_LOGO, WLR_MODIFIER_LOGO));
  CHECK(!modifierTapPressMatches(WLR_MODIFIER_CTRL, WLR_MODIFIER_LOGO, WLR_MODIFIER_LOGO));
  CHECK(!modifierTapPressMatches(0, WLR_MODIFIER_ALT, WLR_MODIFIER_LOGO));
}

int main() { return RUN_TESTS(); }
