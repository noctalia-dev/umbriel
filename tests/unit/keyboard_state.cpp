#include "input/keyboard_state.h"

#include "check.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <vector>

extern "C" {
#include <wlr/interfaces/wlr_keyboard.h>
}

namespace {

  constexpr std::string_view kModifiers = "NumLock=Mod2, ScrollLock=Mod3, FirstLock=Mod4, SecondLock=Mod5";
  constexpr std::string_view kReorderedModifiers = "SecondLock=Mod4, FirstLock=Mod5, ScrollLock=Mod3, NumLock=Mod2";

  std::string keymapText(std::string_view virtualModifiers, bool shiftIsControl, bool exchangeLocks) {
    std::string text = R"(
xkb_keymap {
    xkb_keycodes "test" {
        minimum = 8;
        maximum = 255;
        <KEYA> = 38;
        <LFSH> = 50;
        <CAPS> = 66;
        <NMLK> = 77;
        <SCLK> = 78;
        <GRUP> = 108;
        indicator 1 = "Caps Lock";
        indicator 2 = "Num Lock";
        indicator 3 = "Scroll Lock";
        indicator 4 = "Compose";
        indicator 5 = "Kana";
    };
    xkb_types "test" {
        virtual_modifiers @MODIFIERS@;
        type "ONE_LEVEL" {
            modifiers = None;
            map[None] = Level1;
        };
    };
    xkb_compatibility "test" {
        interpret Shift_L { action = SetMods(modifiers = Shift); };
        interpret Control_L { action = SetMods(modifiers = Control); };
        interpret Caps_Lock { action = LockMods(modifiers = Lock); };
        interpret Num_Lock { action = LockMods(modifiers = @NUM_MODIFIER@); };
        interpret Scroll_Lock { action = LockMods(modifiers = @SCROLL_MODIFIER@); };
        interpret Mode_switch { action = SetGroup(group = +1); };
        indicator "Caps Lock" { whichModState = locked; modifiers = Lock; };
        indicator "Num Lock" { whichModState = locked; modifiers = @NUM_MODIFIER@; };
        indicator "Scroll Lock" { whichModState = locked; modifiers = @SCROLL_MODIFIER@; };
        indicator "Compose" { modifiers = None; };
        indicator "Kana" { modifiers = None; };
    };
    xkb_symbols "test" {
        name[group1] = "First";
        name[group2] = "Second";
        key <KEYA> { type = "ONE_LEVEL", symbols[group1] = [ a ], symbols[group2] = [ b ] };
        key <LFSH> { type = "ONE_LEVEL", [ @SHIFT_KEY@ ] };
        key <CAPS> { type = "ONE_LEVEL", [ Caps_Lock ] };
        key <NMLK> { type = "ONE_LEVEL", [ Num_Lock ] };
        key <SCLK> { type = "ONE_LEVEL", [ Scroll_Lock ] };
        key <GRUP> { type = "ONE_LEVEL", [ Mode_switch ] };
    };
};
)";
    text.replace(text.find("@MODIFIERS@"), std::string_view("@MODIFIERS@").size(), virtualModifiers);
    text.replace(
        text.find("@SHIFT_KEY@"), std::string_view("@SHIFT_KEY@").size(), shiftIsControl ? "Control_L" : "Shift_L"
    );
    for (const std::string_view placeholder : {"@NUM_MODIFIER@", "@SCROLL_MODIFIER@"}) {
      const bool numLock = placeholder == "@NUM_MODIFIER@";
      const std::string_view replacement = numLock != exchangeLocks ? "Mod2" : "Mod3";
      size_t position = 0;
      while ((position = text.find(placeholder, position)) != std::string::npos) {
        text.replace(position, placeholder.size(), replacement);
        position += replacement.size();
      }
    }
    return text;
  }

  class Keymap {
  public:
    explicit Keymap(
        std::string_view virtualModifiers = kModifiers, bool shiftIsControl = false, bool exchangeLocks = false
    ) {
      xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      const std::string text = keymapText(virtualModifiers, shiftIsControl, exchangeLocks);
      m_keymap = context != nullptr
          ? xkb_keymap_new_from_string(context, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS)
          : nullptr;
      xkb_context_unref(context);
      if (m_keymap == nullptr) {
        throw std::runtime_error("could not compile the keyboard state test keymap");
      }
    }

    ~Keymap() { xkb_keymap_unref(m_keymap); }
    Keymap(const Keymap&) = delete;
    Keymap& operator=(const Keymap&) = delete;
    [[nodiscard]] xkb_keymap* get() const { return m_keymap; }

  private:
    xkb_keymap* m_keymap = nullptr;
  };

  xkb_mod_mask_t modifierMask(xkb_keymap* keymap, const char* name) {
    const xkb_mod_mask_t mask = xkb_keymap_mod_get_mask(keymap, name);
    if (mask == 0) {
      throw std::runtime_error("keyboard state test keymap lacks the requested modifier");
    }
    return mask;
  }

  // The fixture supplies only a backend LED callback. Key replay, XKB actions,
  // modifier signals, and failure to allocate the keymap's fd all use wlroots.
  class BackendKeyboard {
  public:
    explicit BackendKeyboard(xkb_keymap* keymap) {
      wlr_keyboard_init(&m_keyboard, &kImpl, "keyboard state test");
      m_keyboard.data = this;
      if (!wlr_keyboard_set_keymap(&m_keyboard, keymap)) {
        wlr_keyboard_finish(&m_keyboard);
        throw std::runtime_error("could not apply the keyboard state test keymap");
      }
      m_modifiers.notify = onModifiers;
      wl_signal_add(&m_keyboard.events.modifiers, &m_modifiers);
    }

    ~BackendKeyboard() {
      wl_list_remove(&m_modifiers.link);
      wlr_keyboard_finish(&m_keyboard);
    }

    BackendKeyboard(const BackendKeyboard&) = delete;
    BackendKeyboard& operator=(const BackendKeyboard&) = delete;
    [[nodiscard]] wlr_keyboard* get() { return &m_keyboard; }
    [[nodiscard]] const std::vector<uint32_t>& leds() const { return m_leds; }
    [[nodiscard]] const std::vector<wlr_keyboard_modifiers>& modifiers() const { return m_modifierEvents; }

    void key(uint32_t code, wl_keyboard_key_state state) {
      wlr_keyboard_key_event event{.time_msec = ++m_time, .keycode = code, .update_state = true, .state = state};
      wlr_keyboard_notify_key(&m_keyboard, &event);
    }

    void tap(uint32_t code) {
      key(code, WL_KEYBOARD_KEY_STATE_PRESSED);
      key(code, WL_KEYBOARD_KEY_STATE_RELEASED);
    }

  private:
    static void recordLeds(wlr_keyboard* keyboard, uint32_t leds) {
      static_cast<BackendKeyboard*>(keyboard->data)->m_leds.push_back(leds);
    }

    static void onModifiers(wl_listener* listener, void* /*data*/) {
      BackendKeyboard* self;
      self = wl_container_of(listener, self, m_modifiers);
      self->m_modifierEvents.push_back(self->m_keyboard.modifiers);
    }

    static constexpr wlr_keyboard_impl kImpl{.name = "keyboard state backend", .led_update = recordLeds};
    wlr_keyboard m_keyboard{};
    wl_listener m_modifiers{};
    std::vector<uint32_t> m_leds;
    std::vector<wlr_keyboard_modifiers> m_modifierEvents;
    uint32_t m_time = 0;
  };

  class ClosedFdBudget {
  public:
    ClosedFdBudget() {
      if (getrlimit(RLIMIT_NOFILE, &m_previous) != 0) {
        throw std::runtime_error("could not read the test file descriptor limit");
      }
      rlimit closed = m_previous;
      closed.rlim_cur = 0;
      if (setrlimit(RLIMIT_NOFILE, &closed) != 0) {
        throw std::runtime_error("could not set the test file descriptor limit");
      }
    }

    ~ClosedFdBudget() { setrlimit(RLIMIT_NOFILE, &m_previous); }
    ClosedFdBudget(const ClosedFdBudget&) = delete;
    ClosedFdBudget& operator=(const ClosedFdBudget&) = delete;

  private:
    rlimit m_previous{};
  };

} // namespace

UMBRIEL_TEST(remapsModifierNamesWhenVirtualModifierIndicesChange) {
  Keymap original;
  Keymap reordered(kReorderedModifiers);
  const xkb_mod_mask_t before = modifierMask(original.get(), "FirstLock");
  const xkb_mod_mask_t after = modifierMask(reordered.get(), "FirstLock");
  CHECK(before != after);
  const auto names = umbriel::keyboardModifierNames(original.get(), before);
  CHECK_EQ(names, std::vector<std::string>{"FirstLock"});
  CHECK_EQ(umbriel::keyboardModifierMask(reordered.get(), names), after);
  CHECK_EQ(umbriel::keyboardModifierMask(reordered.get(), std::vector<std::string>{"MissingLock"}), 0U);
}

UMBRIEL_TEST(preservesCapsNumAndScrollAcrossKeymapReplacement) {
  Keymap original;
  Keymap replacement(kReorderedModifiers);
  BackendKeyboard keyboard(original.get());
  keyboard.tap(58);
  keyboard.tap(69);
  keyboard.tap(70);
  const xkb_mod_mask_t locked = keyboard.get()->modifiers.locked;
  const uint32_t allLeds = WLR_LED_CAPS_LOCK | WLR_LED_NUM_LOCK | WLR_LED_SCROLL_LOCK;
  CHECK_EQ(
      locked,
      modifierMask(original.get(), "Lock") | modifierMask(original.get(), "Mod2") | modifierMask(original.get(), "Mod3")
  );
  CHECK_EQ(keyboard.leds().back(), allLeds);
  const size_t events = keyboard.modifiers().size();

  CHECK(umbriel::setKeyboardKeymapPreservingState(keyboard.get(), replacement.get()));
  CHECK_EQ(keyboard.get()->modifiers.locked, locked);
  CHECK_EQ(keyboard.leds().back(), allLeds);
  CHECK_EQ(keyboard.modifiers().size(), events + 1);
  CHECK_EQ(keyboard.modifiers().back().locked, locked);
  keyboard.tap(69);
  CHECK_EQ(keyboard.get()->modifiers.locked, locked & ~modifierMask(replacement.get(), "Mod2"));
  CHECK_EQ(keyboard.leds().back(), uint32_t{WLR_LED_CAPS_LOCK | WLR_LED_SCROLL_LOCK});
}

UMBRIEL_TEST(followsNumLockWhenItsBackingRealModifierChanges) {
  Keymap original;
  Keymap replacement("ScrollLock=Mod2, NumLock=Mod3, FirstLock=Mod4, SecondLock=Mod5", false, true);
  BackendKeyboard keyboard(original.get());
  keyboard.tap(69);
  const auto names = umbriel::keyboardModifierNames(original.get(), keyboard.get()->modifiers.locked);
  CHECK_EQ(names, std::vector<std::string>{"NumLock"});

  CHECK(umbriel::setKeyboardKeymapPreservingState(keyboard.get(), replacement.get()));
  CHECK_EQ(keyboard.get()->modifiers.locked, modifierMask(replacement.get(), "NumLock"));
  CHECK_EQ(keyboard.get()->modifiers.locked & modifierMask(replacement.get(), "ScrollLock"), 0U);
  CHECK_EQ(keyboard.leds().back(), uint32_t{WLR_LED_NUM_LOCK});
  keyboard.tap(69);
  CHECK_EQ(keyboard.get()->modifiers.locked, 0U);
  CHECK_EQ(keyboard.leds().back(), 0U);
}

UMBRIEL_TEST(anUnchangedKeymapRetainsItsStateAndPendingActions) {
  Keymap original;
  Keymap identical;
  BackendKeyboard keyboard(original.get());
  keyboard.tap(69);
  keyboard.key(42, WL_KEYBOARD_KEY_STATE_PRESSED);
  umbriel::setKeyboardLatchedLockedModifiers(
      keyboard.get(), modifierMask(original.get(), "Control"), keyboard.get()->modifiers.locked
  );
  xkb_state* state = keyboard.get()->xkb_state;
  const size_t events = keyboard.modifiers().size();

  CHECK(umbriel::setKeyboardKeymapPreservingState(keyboard.get(), identical.get()));
  CHECK_EQ(keyboard.get()->xkb_state, state);
  CHECK_EQ(keyboard.modifiers().size(), events);
  CHECK_EQ(keyboard.get()->modifiers.latched, modifierMask(original.get(), "Control"));
  keyboard.tap(30);
  CHECK_EQ(keyboard.get()->modifiers.latched, 0U);
  CHECK_EQ(keyboard.get()->modifiers.depressed, modifierMask(original.get(), "Shift"));
  keyboard.key(42, WL_KEYBOARD_KEY_STATE_RELEASED);
  CHECK_EQ(keyboard.get()->modifiers.depressed, 0U);
  CHECK_EQ(keyboard.get()->modifiers.locked, modifierMask(original.get(), "NumLock"));
}

UMBRIEL_TEST(releasesHeldModifiersAfterRestoringLocks) {
  Keymap original;
  Keymap replacement(kReorderedModifiers);
  BackendKeyboard keyboard(original.get());
  keyboard.tap(69);
  keyboard.key(42, WL_KEYBOARD_KEY_STATE_PRESSED);
  CHECK_EQ(keyboard.get()->modifiers.depressed, modifierMask(original.get(), "Shift"));

  CHECK(umbriel::setKeyboardKeymapPreservingState(keyboard.get(), replacement.get()));
  CHECK_EQ(keyboard.get()->modifiers.depressed, modifierMask(replacement.get(), "Shift"));
  CHECK_EQ(keyboard.get()->modifiers.locked, modifierMask(replacement.get(), "Mod2"));
  keyboard.key(42, WL_KEYBOARD_KEY_STATE_RELEASED);
  CHECK_EQ(keyboard.get()->modifiers.depressed, 0U);
  CHECK_EQ(keyboard.get()->modifiers.locked, modifierMask(replacement.get(), "Mod2"));
}

UMBRIEL_TEST(replaysHeldKeysWithTheirReplacementKeymapActions) {
  Keymap original;
  Keymap replacement(kModifiers, true);
  BackendKeyboard keyboard(original.get());
  keyboard.tap(69);
  keyboard.key(42, WL_KEYBOARD_KEY_STATE_PRESSED);

  CHECK(umbriel::setKeyboardKeymapPreservingState(keyboard.get(), replacement.get()));
  CHECK_EQ(keyboard.get()->modifiers.depressed, modifierMask(replacement.get(), "Control"));
  CHECK_EQ(keyboard.get()->modifiers.locked, modifierMask(replacement.get(), "Mod2"));
  keyboard.key(42, WL_KEYBOARD_KEY_STATE_RELEASED);
  CHECK_EQ(keyboard.get()->modifiers.depressed, 0U);
}

UMBRIEL_TEST(preservesLatchedModifiersAndLetsTheNextKeyConsumeThem) {
  Keymap original;
  Keymap replacement(kReorderedModifiers);
  BackendKeyboard keyboard(original.get());
  umbriel::setKeyboardLatchedLockedModifiers(keyboard.get(), modifierMask(original.get(), "Shift"), 0);
  CHECK_EQ(keyboard.get()->modifiers.latched, modifierMask(original.get(), "Shift"));

  CHECK(umbriel::setKeyboardKeymapPreservingState(keyboard.get(), replacement.get()));
  CHECK_EQ(keyboard.get()->modifiers.latched, modifierMask(replacement.get(), "Shift"));
  keyboard.tap(30);
  CHECK_EQ(keyboard.get()->modifiers.latched, 0U);
}

UMBRIEL_TEST(restoringLocksPreservesAHeldGroupSwitchUntilItsRelease) {
  Keymap original;
  Keymap replacement(kReorderedModifiers);
  BackendKeyboard keyboard(original.get());
  keyboard.tap(69);
  keyboard.key(100, WL_KEYBOARD_KEY_STATE_PRESSED);
  CHECK_EQ(keyboard.get()->modifiers.group, 1U);
  CHECK_EQ(xkb_state_serialize_layout(keyboard.get()->xkb_state, XKB_STATE_LAYOUT_DEPRESSED), 1U);

  CHECK(umbriel::setKeyboardKeymapPreservingState(keyboard.get(), replacement.get()));
  CHECK_EQ(keyboard.get()->modifiers.group, 1U);
  CHECK_EQ(xkb_state_serialize_layout(keyboard.get()->xkb_state, XKB_STATE_LAYOUT_DEPRESSED), 1U);
  CHECK_EQ(xkb_state_serialize_layout(keyboard.get()->xkb_state, XKB_STATE_LAYOUT_LOCKED), 0U);
  keyboard.key(100, WL_KEYBOARD_KEY_STATE_RELEASED);
  CHECK_EQ(keyboard.get()->modifiers.group, 0U);
  CHECK_EQ(keyboard.get()->modifiers.locked, modifierMask(replacement.get(), "Mod2"));
}

UMBRIEL_TEST(keymapReplacementKeepsTheExistingLayoutResetPolicy) {
  Keymap original;
  Keymap replacement(kReorderedModifiers);
  BackendKeyboard keyboard(original.get());
  wlr_keyboard_notify_modifiers(keyboard.get(), 0, 0, modifierMask(original.get(), "Mod2"), 1);
  CHECK_EQ(keyboard.get()->modifiers.group, 1U);

  CHECK(umbriel::setKeyboardKeymapPreservingState(keyboard.get(), replacement.get()));
  CHECK_EQ(keyboard.get()->modifiers.group, 0U);
  CHECK_EQ(keyboard.get()->modifiers.locked, modifierMask(replacement.get(), "Mod2"));
}

UMBRIEL_TEST(aFailedKeymapReplacementKeepsTheOriginalState) {
  Keymap original;
  Keymap replacement(kReorderedModifiers);
  BackendKeyboard keyboard(original.get());
  keyboard.tap(69);
  keyboard.key(42, WL_KEYBOARD_KEY_STATE_PRESSED);
  xkb_state* state = keyboard.get()->xkb_state;
  xkb_keymap* keymap = keyboard.get()->keymap;
  const wlr_keyboard_modifiers before = keyboard.get()->modifiers;
  const size_t events = keyboard.modifiers().size();
  bool applied = true;
  {
    ClosedFdBudget noNewKeymapFd;
    applied = umbriel::setKeyboardKeymapPreservingState(keyboard.get(), replacement.get());
  }
  CHECK(!applied);
  CHECK_EQ(keyboard.get()->keymap, keymap);
  CHECK_EQ(keyboard.get()->xkb_state, state);
  CHECK_EQ(keyboard.get()->modifiers.depressed, before.depressed);
  CHECK_EQ(keyboard.get()->modifiers.latched, before.latched);
  CHECK_EQ(keyboard.get()->modifiers.locked, before.locked);
  CHECK_EQ(keyboard.get()->modifiers.group, before.group);
  CHECK_EQ(keyboard.modifiers().size(), events);
  keyboard.key(42, WL_KEYBOARD_KEY_STATE_RELEASED);
  CHECK_EQ(keyboard.get()->modifiers.depressed, 0U);
  CHECK_EQ(keyboard.get()->modifiers.locked, before.locked);
}

UMBRIEL_TEST(rejectsANullKeymapWithoutClearingTheCurrentState) {
  Keymap original;
  BackendKeyboard keyboard(original.get());
  keyboard.tap(69);
  xkb_state* state = keyboard.get()->xkb_state;

  CHECK(!umbriel::setKeyboardKeymapPreservingState(keyboard.get(), nullptr));
  CHECK_EQ(keyboard.get()->xkb_state, state);
  CHECK_EQ(keyboard.get()->keymap, original.get());
  CHECK_EQ(keyboard.get()->modifiers.locked, modifierMask(original.get(), "Mod2"));
}

int main() { return RUN_TESTS(); }
