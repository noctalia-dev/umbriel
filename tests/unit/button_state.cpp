#include "input/button_state.h"

#include "check.h"

#include <linux/input-event-codes.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

using umbriel::ButtonState;
using umbriel::Keybind;
using umbriel::KeybindAction;

namespace {
  Keybind releaseBind() {
    Keybind bind;
    bind.useMod = true;
    bind.onRelease = true;
    bind.repeat = false;
    bind.mouseButton = BTN_SIDE;
    bind.action = KeybindAction::OverviewClose;
    return bind;
  }

  Keybind keyboardReleaseBind() {
    Keybind bind = releaseBind();
    bind.mouseButton = 0;
    bind.keysym = XKB_KEY_h;
    return bind;
  }

  Keybind modifierTapBind() {
    Keybind bind;
    bind.useMod = true;
    bind.modifierOnly = true;
    bind.repeat = false;
    bind.action = KeybindAction::OverviewClose;
    return bind;
  }

  struct ReleaseCase {
    Keybind bind;
    uint32_t code;
    uint32_t otherCode;
  };

  std::vector<ReleaseCase> releaseCases() {
    return {
        {releaseBind(), BTN_SIDE, BTN_BACK},
        {keyboardReleaseBind(), KEY_H, KEY_J},
        {modifierTapBind(), KEY_LEFTMETA, KEY_RIGHTMETA},
    };
  }
} // namespace

UMBRIEL_TEST(releaseSnapshotsAreOneShotAndScopedToSourceAndDeliveredCode) {
  for (const auto& [bind, code, otherCode] : releaseCases()) {
    ButtonState state;
    int firstSource = 0;
    int secondSource = 0;
    int unknownSource = 0;
    Keybind original = bind;
    for (const auto* source : {&firstSource, &secondSource}) {
      for (const auto deliveredCode : {code, otherCode}) {
        state.armRelease(source, deliveredCode, original);
      }
    }
    original.action = KeybindAction::OverviewOpen;
    CHECK(!state.takeRelease(&unknownSource, code));
    CHECK(!state.takeRelease(&firstSource, KEY_RESERVED));
    for (const auto* source : {&firstSource, &secondSource}) {
      for (const auto deliveredCode : {code, otherCode}) {
        CHECK(state.releaseBind(source, deliveredCode) != nullptr);
        CHECK(state.takeRelease(source, deliveredCode) == std::optional<Keybind>{bind});
        CHECK(state.releaseBind(source, deliveredCode) == nullptr);
        CHECK(!state.takeRelease(source, deliveredCode));
      }
    }
  }
}

UMBRIEL_TEST(cancellationAndReloadsDoNotReviveActions) {
  for (const auto& [bind, code, otherCode] : releaseCases()) {
    ButtonState state;
    int firstSource = 0;
    int secondSource = 0;
    state.armRelease(&firstSource, code, bind);
    state.armRelease(&firstSource, otherCode, bind);
    state.armRelease(&secondSource, code, bind);
    state.forgetSource(&firstSource);
    CHECK(!state.takeRelease(&firstSource, code));
    CHECK(!state.takeRelease(&firstSource, otherCode));
    state.reconcileReleaseBinds(std::vector<Keybind>{bind});
    CHECK(state.takeRelease(&secondSource, code) == std::optional<Keybind>{bind});

    Keybind replacement = bind;
    replacement.action = KeybindAction::OverviewOpen;
    for (const auto& changed : {std::vector<Keybind>{}, std::vector<Keybind>{replacement}}) {
      state.armRelease(&firstSource, code, bind);
      state.reconcileReleaseBinds(changed);
      state.reconcileReleaseBinds(std::vector<Keybind>{bind});
      CHECK(!state.takeRelease(&firstSource, code));
    }
    state.armRelease(&firstSource, code, bind);
    state.clear();
    CHECK(!state.takeRelease(&firstSource, code));
  }
}

UMBRIEL_TEST(otherInputCancelsOnlyModifierOnlyReleaseActions) {
  ButtonState state;
  CHECK(!state.cancelForInput());
  int keyboard = 0;
  int pointer = 0;
  const Keybind tap = modifierTapBind();
  const Keybind keyBind = keyboardReleaseBind();
  const Keybind bind = releaseBind();
  state.armRelease(&keyboard, KEY_LEFTMETA, tap);
  state.armRelease(&keyboard, KEY_H, keyBind);
  state.armRelease(&pointer, BTN_SIDE, bind);
  CHECK(state.cancelForInput());
  CHECK(!state.cancelForInput());
  CHECK(state.releaseBind(&keyboard, KEY_LEFTMETA) == nullptr);
  CHECK(!state.takeRelease(&keyboard, KEY_LEFTMETA));
  CHECK(state.takeRelease(&keyboard, KEY_H) == std::optional<Keybind>{keyBind});
  CHECK(state.takeRelease(&pointer, BTN_SIDE) == std::optional<Keybind>{bind});
}

UMBRIEL_TEST(forgettingAPressLeavesOtherSourcesAndCodesArmed) {
  ButtonState state;
  int pointer = 0;
  int tool = 0;
  const Keybind bind = releaseBind();
  state.armRelease(&pointer, BTN_SIDE, bind);
  state.armRelease(&pointer, BTN_BACK, bind);
  state.armRelease(&tool, BTN_SIDE, bind);
  state.forgetPress(&pointer, BTN_SIDE);
  CHECK(!state.takeRelease(&pointer, BTN_SIDE));
  CHECK(state.takeRelease(&pointer, BTN_BACK) == std::optional<Keybind>{bind});
  CHECK(state.takeRelease(&tool, BTN_SIDE) == std::optional<Keybind>{bind});
}

int main() { return RUN_TESTS(); }
