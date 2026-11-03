#include "input/shortcut_keysym.h"

#include "check.h"

#include <cstdint>
#include <linux/input-event-codes.h>

using umbriel::rawShortcutKeysym;

namespace {

  class Keymap {
  public:
    explicit Keymap(const char* layouts) : m_context(xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {
      const xkb_rule_names names{
          .rules = nullptr,
          .model = nullptr,
          .layout = layouts,
          .variant = nullptr,
          .options = nullptr,
      };
      if (m_context != nullptr) {
        m_keymap = xkb_keymap_new_from_names(m_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
      }
    }

    ~Keymap() {
      if (m_keymap != nullptr) {
        xkb_keymap_unref(m_keymap);
      }
      if (m_context != nullptr) {
        xkb_context_unref(m_context);
      }
    }

    Keymap(const Keymap&) = delete;
    Keymap& operator=(const Keymap&) = delete;

    xkb_keymap* get() const { return m_keymap; }

  private:
    xkb_context* m_context = nullptr;
    xkb_keymap* m_keymap = nullptr;
  };

  xkb_keysym_t levelZero(xkb_keymap* keymap, xkb_keycode_t keycode, xkb_layout_index_t layout) {
    const xkb_keysym_t* keysyms = nullptr;
    const int count = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, 0, &keysyms);
    return count > 0 && keysyms != nullptr ? keysyms[0] : XKB_KEY_NoSymbol;
  }

  constexpr xkb_keycode_t xkbKeycode(uint32_t evdevKeycode) { return evdevKeycode + 8; }

} // namespace

UMBRIEL_TEST(fallsBackToAsciiFromAnotherConfiguredLayout) {
  Keymap keymap("us,de,ru");
  CHECK(keymap.get() != nullptr);
  if (keymap.get() == nullptr) {
    return;
  }

  const xkb_keycode_t keycode = xkbKeycode(KEY_A);
  CHECK_EQ(levelZero(keymap.get(), keycode, 2), xkb_keysym_t{XKB_KEY_Cyrillic_ef});
  CHECK_EQ(rawShortcutKeysym(keymap.get(), keycode, 2), xkb_keysym_t{XKB_KEY_a});
}

UMBRIEL_TEST(keepsAsciiSymbolFromActiveLayout) {
  Keymap keymap("us,de,ru");
  CHECK(keymap.get() != nullptr);
  if (keymap.get() == nullptr) {
    return;
  }

  const xkb_keycode_t keycode = xkbKeycode(KEY_Y);
  CHECK_EQ(levelZero(keymap.get(), keycode, 1), xkb_keysym_t{XKB_KEY_z});
  CHECK_EQ(rawShortcutKeysym(keymap.get(), keycode, 1), xkb_keysym_t{XKB_KEY_z});
}

UMBRIEL_TEST(doesNotInventAnUnconfiguredLatinLayout) {
  Keymap keymap("ru");
  CHECK(keymap.get() != nullptr);
  if (keymap.get() == nullptr) {
    return;
  }

  const xkb_keycode_t keycode = xkbKeycode(KEY_A);
  CHECK_EQ(levelZero(keymap.get(), keycode, 0), xkb_keysym_t{XKB_KEY_Cyrillic_ef});
  CHECK_EQ(rawShortcutKeysym(keymap.get(), keycode, 0), xkb_keysym_t{XKB_KEY_Cyrillic_ef});
}

int main() { return RUN_TESTS(); }
