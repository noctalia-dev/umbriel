#include "config/config.h"
#include "input/cursor.h"
#include "input/keyboard.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "server/actions.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

#include <string_view>
#include <utility>

namespace umbriel {
  namespace {
    uint32_t modifierMaskForKeysym(uint32_t keysym) {
      switch (keysym) {
      case XKB_KEY_Shift_L:
      case XKB_KEY_Shift_R:
        return WLR_MODIFIER_SHIFT;
      case XKB_KEY_Control_L:
      case XKB_KEY_Control_R:
        return WLR_MODIFIER_CTRL;
      case XKB_KEY_Alt_L:
      case XKB_KEY_Alt_R:
        return WLR_MODIFIER_ALT;
      case XKB_KEY_Super_L:
      case XKB_KEY_Super_R:
        return WLR_MODIFIER_LOGO;
      default:
        return 0;
      }
    }
  } // namespace

  bool Server::handleVtSwitch(uint32_t keysym, uint32_t modifiers) {
    if (m_session == nullptr) {
      return false;
    }

    unsigned vt = 0;
    if (keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
      vt = 1 + (keysym - XKB_KEY_XF86Switch_VT_1);
    } else if (
        (modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) == (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)
        && keysym >= XKB_KEY_F1
        && keysym <= XKB_KEY_F12
    ) {
      vt = 1 + (keysym - XKB_KEY_F1);
    }

    if (vt == 0) {
      return false;
    }

    if (!wlr_session_change_vt(m_session, vt)) {
      wlr_log(WLR_ERROR, "failed to switch to VT %u", vt);
    }
    return true;
  }

  bool Server::executeKeybindAction(const Keybind& bind, std::string* error) {
    if (error != nullptr) {
      error->clear();
    }
    if (bind.action == KeybindAction::None) {
      return false;
    }
    const ActionHandlerFn handler = actionHandlerFor(bind.action);
    if (handler == nullptr) {
      if (error != nullptr) {
        *error = "action has no handler";
      }
      return false;
    }
    // Config reload may replace the keybind storage while its action runs, so
    // retain the transition before dispatching the primary action.
    const std::optional<SubmapArg> submapAfter = bind.submapAfter;
    const bool handled = handler(*this, bind, error);
    if (!submapAfter.has_value()) {
      return handled;
    }
    if (isSubmapReset(*submapAfter)) {
      if (inSubmap()) {
        popSubmap();
      }
    } else {
      pushSubmap(submapAfter->name);
    }
    return true;
  }

  void
  Server::armModifierTap(const void* source, uint32_t keycode, std::span<const uint32_t> keysyms, uint32_t modifiers) {
    if (m_modifierTap.cancelForKeyPress()) {
      return;
    }

    uint32_t pressedModifier = 0;
    for (const uint32_t keysym : keysyms) {
      pressedModifier |= modifierMaskForKeysym(keysym);
    }
    if (pressedModifier == 0) {
      return;
    }

    const std::string_view currentSubmap = m_activeSubmaps.empty() ? std::string_view{} : m_activeSubmaps.back();
    for (const Keybind& bind : config().keybinds) {
      if (!bind.modifierOnly) {
        continue;
      }
      if (m_sessionLocked && !bind.allowWhenLocked) {
        continue;
      }
      if (bind.submap != currentSubmap) {
        if (m_activeSubmaps.empty() || !bind.submap.empty() || !isSubmapResetBind(bind)) {
          continue;
        }
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (modifierTapPressMatches(modifiers, pressedModifier, expected)) {
        m_modifierTap.arm(bind, source, keycode);
        return;
      }
    }
  }

  std::optional<Keybind> Server::releaseModifierTap(const void* source, uint32_t keycode) {
    std::optional<Keybind> bind = m_modifierTap.release(source, keycode);
    if (bind && m_sessionLocked && !bind->allowWhenLocked) {
      return std::nullopt;
    }
    return bind;
  }

  const Keybind* Server::matchKeybind(uint32_t keysym, uint32_t rawKeysym, uint32_t modifiers) const {
    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const uint32_t lowered = xkb_keysym_to_lower(keysym);
    const std::string_view currentSubmap = m_activeSubmaps.empty() ? std::string_view{} : m_activeSubmaps.back();

    for (const Keybind& bind : config().keybinds) {
      if (bind.submap != currentSubmap) {
        // Allow submap:reset from the default context to always match, so users
        // can define a global emergency exit.
        if (!m_activeSubmaps.empty() && bind.submap.empty() && isSubmapResetBind(bind)) {
          // Fall through to match below.
        } else {
          continue;
        }
      }
      if (m_sessionLocked && !bind.allowWhenLocked) {
        continue;
      }
      if (bind.modifierOnly) {
        continue;
      }
      if (bind.wheel != WheelDirection::None || bind.mouseButton != 0) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected || (lowered != bind.keysym && rawKeysym != bind.keysym)) {
        continue;
      }
      return &bind;
    }

    return nullptr;
  }

  uint32_t Server::keyboardModifiers() const {
    uint32_t modifiers = 0;
    for (const auto& keyboard : m_keyboards) {
      if (keyboard != nullptr && keyboard->wlr() != nullptr) {
        modifiers |= wlr_keyboard_get_modifiers(keyboard->wlr());
      }
    }
    return modifiers;
  }

  std::optional<Keybind> Server::handleKeybind(uint32_t keysym, uint32_t rawKeysym, uint32_t modifiers) {
    const Keybind* bind = matchKeybind(keysym, rawKeysym, modifiers);
    if (bind == nullptr) {
      return std::nullopt;
    }
    const Keybind matched = *bind;
    return executeKeybindAction(matched) ? std::optional<Keybind>{matched} : std::nullopt;
  }

  bool Server::handleWheelBind(WheelDirection direction, uint32_t modifiers) {
    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const std::string_view currentSubmap = m_activeSubmaps.empty() ? std::string_view{} : m_activeSubmaps.back();

    for (const Keybind& bind : config().keybinds) {
      if (bind.submap != currentSubmap) {
        continue;
      }
      if (m_sessionLocked && !bind.allowWhenLocked) {
        continue;
      }
      if (bind.wheel != direction) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected) {
        continue;
      }
      return executeKeybindAction(bind);
    }

    return false;
  }

  std::optional<Keybind> Server::handleMouseBind(uint32_t button, uint32_t modifiers) {
    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const std::string_view currentSubmap = m_activeSubmaps.empty() ? std::string_view{} : m_activeSubmaps.back();

    for (const Keybind& bind : config().keybinds) {
      if (bind.submap != currentSubmap) {
        continue;
      }
      if (m_sessionLocked && !bind.allowWhenLocked) {
        continue;
      }
      // Non-mouse binds carry 0 here, which never equals a BTN_* code.
      if (bind.mouseButton != button) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected) {
        continue;
      }
      std::optional<Keybind> matched{bind};
      const bool handled = executeKeybindAction(*matched);
      return handled ? std::move(matched) : std::nullopt;
    }

    return std::nullopt;
  }

  void Server::pushSubmap(const std::string& name) {
    cancelModifierTap();
    m_activeSubmaps.push_back(name);
  }

  void Server::popSubmap() {
    cancelModifierTap();
    if (!m_activeSubmaps.empty()) {
      m_activeSubmaps.pop_back();
    }
  }

} // namespace umbriel
