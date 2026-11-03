#include "input/keyboard_state.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <string_view>

extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

namespace umbriel {

  namespace {
    constexpr std::array<std::string_view, 8> kRealModifiers{"Shift", "Lock", "Control", "Mod1",
                                                             "Mod2",  "Mod3", "Mod4",    "Mod5"};

    bool realModifier(std::string_view name) { return std::ranges::contains(kRealModifiers, name); }
  } // namespace

  std::vector<std::string> keyboardModifierNames(xkb_keymap* keymap, xkb_mod_mask_t mask) {
    std::vector<std::string> names;
    if (keymap == nullptr) {
      return names;
    }
    const auto take = [&](const char* name) {
      const xkb_mod_mask_t encoding = xkb_keymap_mod_get_mask(keymap, name);
      if (encoding != 0 && (mask & encoding) == encoding) {
        names.emplace_back(name);
        mask &= ~encoding;
      }
    };
    // Prefer a lock's semantic name to its backing real modifier. Keeping
    // both would leave an unrelated modifier locked when a new keymap moves
    // NumLock, for example, from Mod2 to Mod3. Other overlapping virtual
    // mappings are inherently ambiguous, so use their declaration order.
    take("NumLock");
    take("ScrollLock");
    const xkb_mod_index_t count = xkb_keymap_num_mods(keymap);
    for (xkb_mod_index_t index = 0; index < count; ++index) {
      const char* name = xkb_keymap_mod_get_name(keymap, index);
      if (name != nullptr && !realModifier(name)) {
        take(name);
      }
    }
    for (const std::string_view name : kRealModifiers) {
      take(name.data());
    }
    return names;
  }

  xkb_mod_mask_t keyboardModifierMask(xkb_keymap* keymap, std::span<const std::string> names) {
    xkb_mod_mask_t mask = 0;
    if (keymap == nullptr) {
      return mask;
    }
    for (const std::string& name : names) {
      mask |= xkb_keymap_mod_get_mask(keymap, name.c_str());
    }
    return mask;
  }

  void setKeyboardLatchedLockedModifiers(wlr_keyboard* keyboard, xkb_mod_mask_t latched, xkb_mod_mask_t locked) {
    if (keyboard == nullptr || keyboard->xkb_state == nullptr) {
      return;
    }
    xkb_state* state = keyboard->xkb_state;
    // The server API preserves held-key action filters and creates real latch
    // filters, so the next ordinary key still consumes a restored latch.
    // Leave layout components alone, including a held group-switch action.
    const xkb_mod_mask_t currentLatched = xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED);
    const xkb_mod_mask_t currentLocked = xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED);
    xkb_state_update_latched_locked(
        state, currentLatched ^ latched, latched, false, 0, currentLocked ^ locked, locked, false, 0
    );
    const wlr_keyboard_modifiers modifiers{
        .depressed = xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED),
        .latched = xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED),
        .locked = xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED),
        .group = xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE),
    };
    const wlr_keyboard_modifiers previous = keyboard->modifiers;
    keyboard->modifiers = modifiers;
    if (previous.depressed != modifiers.depressed
        || previous.latched != modifiers.latched
        || previous.locked != modifiers.locked
        || previous.group != modifiers.group) {
      wl_signal_emit_mutable(&keyboard->events.modifiers, keyboard);
    }
    if (keyboard->xkb_state == nullptr) {
      return;
    }
    uint32_t leds = 0;
    for (uint32_t index = 0; index < WLR_LED_COUNT; ++index) {
      const xkb_led_index_t indicator = keyboard->led_indexes[index];
      if (indicator != XKB_LED_INVALID && xkb_state_led_index_is_active(keyboard->xkb_state, indicator) > 0) {
        leds |= uint32_t{1} << index;
      }
    }
    wlr_keyboard_led_update(keyboard, leds);
  }

  bool setKeyboardKeymapPreservingState(wlr_keyboard* keyboard, xkb_keymap* keymap) {
    if (keyboard == nullptr || keymap == nullptr) {
      return false;
    }
    // Recreating an unchanged keymap would discard pending XKB actions even
    // though only an unrelated input setting, such as mouse sensitivity, may
    // have changed. Keep the original state and its held-key/latch filters.
    if (keyboard->keymap == keymap) {
      return true;
    }
    if (keyboard->keymap_string != nullptr) {
      char* serialized = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
      if (serialized == nullptr) {
        return false;
      }
      const bool unchanged = keyboard->keymap_string == std::string_view(serialized);
      std::free(serialized);
      if (unchanged) {
        return true;
      }
    }
    const bool hadState = keyboard->xkb_state != nullptr;
    const std::vector<std::string> latched = keyboardModifierNames(keyboard->keymap, keyboard->modifiers.latched);
    const std::vector<std::string> locked = keyboardModifierNames(keyboard->keymap, keyboard->modifiers.locked);
    if (!wlr_keyboard_set_keymap(keyboard, keymap)) {
      return false;
    }
    if (hadState) {
      setKeyboardLatchedLockedModifiers(
          keyboard, keyboardModifierMask(keyboard->keymap, latched), keyboardModifierMask(keyboard->keymap, locked)
      );
    }
    return true;
  }

} // namespace umbriel
