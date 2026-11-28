#include "input/global_shortcut_trigger.h"

#include <cmath>
extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

#include <array>
#include <utility>

namespace umbriel {
  namespace {
    constexpr std::array<std::pair<std::string_view, uint32_t>, 5> kModifiers{{
        {"CTRL", WLR_MODIFIER_CTRL},
        {"ALT", WLR_MODIFIER_ALT},
        {"SHIFT", WLR_MODIFIER_SHIFT},
        {"NUM", 0},
        {"LOGO", WLR_MODIFIER_LOGO},
    }};

    bool identifierCharacter(char character) {
      return (character >= 'a' && character <= 'z')
          || (character >= 'A' && character <= 'Z')
          || (character >= '0' && character <= '9')
          || character == '_'
          || character == '+';
    }
  } // namespace

  std::optional<GlobalShortcutTrigger> parseGlobalShortcutTrigger(std::string_view text) {
    size_t length = 0;
    while (length < text.size() && identifierCharacter(text[length])) {
      ++length;
    }
    text = text.substr(0, length);
    GlobalShortcutTrigger trigger;
    for (size_t separator = text.find('+'); separator != std::string_view::npos; separator = text.find('+')) {
      const auto token = text.substr(0, separator);
      bool found = false;
      for (const auto& [name, mask] : kModifiers) {
        if (token == name) {
          trigger.modifiers |= mask;
          found = true;
          break;
        }
      }
      if (!found) {
        return std::nullopt;
      }
      text.remove_prefix(separator + 1);
    }
    if (text.empty()) {
      return std::nullopt;
    }
    const std::string name(text);
    trigger.keysym = xkb_keysym_from_name(name.c_str(), XKB_KEYSYM_NO_FLAGS);
    if (trigger.keysym == XKB_KEY_NoSymbol
        || (trigger.keysym >= XKB_KEY_Shift_L && trigger.keysym <= XKB_KEY_Hyper_R)) {
      return std::nullopt;
    }
    trigger.keysym = xkb_keysym_to_lower(trigger.keysym);
    return trigger;
  }

  bool preferredGlobalShortcutAllowed(const GlobalShortcutTrigger& trigger) {
    if (trigger.keysym == XKB_KEY_NoSymbol) {
      return false;
    }
    if ((trigger.modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)) != 0
        || (trigger.keysym >= XKB_KEY_F1 && trigger.keysym <= XKB_KEY_F35)) {
      return true;
    }
    switch (trigger.keysym) {
    case XKB_KEY_Print:
    case XKB_KEY_Pause:
    case XKB_KEY_Scroll_Lock:
    case XKB_KEY_Menu:
      return true;
    default:
      break;
    }
    std::array<char, 128> name{};
    return xkb_keysym_get_name(trigger.keysym, name.data(), name.size()) > 0
        && std::string_view(name.data()).starts_with("XF86");
  }

  std::string describeGlobalShortcutTrigger(const GlobalShortcutTrigger& trigger) {
    std::array<char, 128> name{};
    if (trigger.keysym == XKB_KEY_NoSymbol
        || xkb_keysym_get_name(xkb_keysym_to_upper(trigger.keysym), name.data(), name.size()) <= 0) {
      return {};
    }
    std::string description;
    for (const auto& [label, mask] : std::array<std::pair<std::string_view, uint32_t>, 4>{{
             {"Ctrl+", WLR_MODIFIER_CTRL},
             {"Alt+", WLR_MODIFIER_ALT},
             {"Shift+", WLR_MODIFIER_SHIFT},
             {"Super+", WLR_MODIFIER_LOGO},
         }}) {
      if ((trigger.modifiers & mask) != 0) {
        description += label;
      }
    }
    description += name.data();
    return description;
  }

} // namespace umbriel
