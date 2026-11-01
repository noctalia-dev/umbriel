#pragma once

#include <span>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon.h>

struct wlr_keyboard;

namespace umbriel {

  // Modifier encodings belong to one keymap. Keep names when carrying state
  // to another keymap, preferring semantic virtual names to backing real
  // modifiers so a moved lock does not also lock its old modifier.
  [[nodiscard]] std::vector<std::string> keyboardModifierNames(xkb_keymap* keymap, xkb_mod_mask_t mask);
  [[nodiscard]] xkb_mod_mask_t keyboardModifierMask(xkb_keymap* keymap, std::span<const std::string> names);

  // Change only the latched and locked modifiers. Preserve held keys and all
  // layout components, and publish the resulting modifiers and LEDs through
  // the same public wlroots fields and signals as notify_modifiers.
  void setKeyboardLatchedLockedModifiers(wlr_keyboard* keyboard, xkb_mod_mask_t latched, xkb_mod_mask_t locked);

  // wlroots replays held keys into the replacement state. Restore latched and
  // locked modifiers by name afterward, leaving that replay's layout intact.
  // An identical serialized keymap retains the existing state and filters.
  // A null replacement or a wlroots failure leaves the old state untouched.
  [[nodiscard]] bool setKeyboardKeymapPreservingState(wlr_keyboard* keyboard, xkb_keymap* keymap);

} // namespace umbriel
