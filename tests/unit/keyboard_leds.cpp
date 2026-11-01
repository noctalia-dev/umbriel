#include "input/keyboard_leds.h"

#include "check.h"
#include "input/keyboard_state.h"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <wlr/interfaces/wlr_keyboard.h>
}

namespace {

  class EventLoop {
  public:
    EventLoop() : m_loop(wl_event_loop_create()) {
      if (m_loop == nullptr) {
        throw std::runtime_error("could not create the keyboard LED test event loop");
      }
    }
    ~EventLoop() { wl_event_loop_destroy(m_loop); }

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    [[nodiscard]] wl_event_loop* get() const { return m_loop; }
    void flush() const { wl_event_loop_dispatch_idle(m_loop); }

  private:
    wl_event_loop* m_loop = nullptr;
  };

  // Only the hardware boundary is supplied here. Key processing, XKB state,
  // modifier/keymap signals, LED deduplication, and idle dispatch are real.
  class BackendKeyboard {
  public:
    explicit BackendKeyboard(bool hasLeds = true, const char* layouts = "us") {
      wlr_keyboard_init(&m_keyboard, hasLeds ? &kWithLeds : &kWithoutLeds, "keyboard LED test");
      m_keyboard.data = this;
      setKeymap(layouts);
      m_key.notify = onKey;
      wl_signal_add(&m_keyboard.events.key, &m_key);
      m_modifiers.notify = onModifiers;
      wl_signal_add(&m_keyboard.events.modifiers, &m_modifiers);
    }

    ~BackendKeyboard() { finish(); }

    BackendKeyboard(const BackendKeyboard&) = delete;
    BackendKeyboard& operator=(const BackendKeyboard&) = delete;

    [[nodiscard]] wlr_keyboard* get() { return &m_keyboard; }
    [[nodiscard]] const std::vector<uint32_t>& writes() const { return m_writes; }
    void clearWrites() { m_writes.clear(); }
    void stopObserving() { m_leds = nullptr; }

    void observe(
        umbriel::KeyboardLeds& leds, bool changesSource = true, bool virtualDevice = false, bool initialNumLock = false
    ) {
      m_leds = &leds;
      m_changesSource = changesSource;
      leds.add(&m_keyboard, virtualDevice, initialNumLock);
    }

    void setKeymap(const char* layouts, bool preserveState = false) {
      xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      const xkb_rule_names names{
          .rules = nullptr, .model = nullptr, .layout = layouts, .variant = nullptr, .options = nullptr
      };
      xkb_keymap* keymap =
          context != nullptr ? xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
      if (keymap == nullptr) {
        xkb_context_unref(context);
        throw std::runtime_error("could not create the keyboard LED test keymap");
      }
      // Config replacement callbacks do not select a fresh input source.
      // Refresh shared locks after the state-preserving helper has returned.
      umbriel::KeyboardLeds* observer = m_leds;
      if (preserveState) {
        m_leds = nullptr;
      }
      const bool applied = preserveState ? umbriel::setKeyboardKeymapPreservingState(&m_keyboard, keymap)
                                         : wlr_keyboard_set_keymap(&m_keyboard, keymap);
      m_leds = observer;
      if (preserveState && m_leds != nullptr) {
        m_leds->update();
      }
      xkb_keymap_unref(keymap);
      xkb_context_unref(context);
      if (!applied) {
        throw std::runtime_error("could not apply the keyboard LED test keymap");
      }
    }

    void renameCapsIndicator() { renameKeymapText("\"Caps Lock\"", "\"Private Caps Indicator\""); }
    void renameNumModifier() { renameKeymapText("NumLock", "PrivateNumLock"); }

    void renameKeymapText(std::string_view original, std::string_view replacement) {
      char* serialized = xkb_keymap_get_as_string(m_keyboard.keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
      if (serialized == nullptr) {
        throw std::runtime_error("could not serialize the keyboard LED test keymap");
      }
      std::string text(serialized);
      std::free(serialized);
      size_t position = 0;
      while ((position = text.find(original, position)) != std::string::npos) {
        text.replace(position, original.size(), replacement);
        position += replacement.size();
      }
      xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      xkb_keymap* keymap = context != nullptr
          ? xkb_keymap_new_from_string(context, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS)
          : nullptr;
      if (keymap == nullptr) {
        xkb_context_unref(context);
        throw std::runtime_error("could not compile the keyboard LED test keymap with a renamed indicator");
      }
      const bool applied = wlr_keyboard_set_keymap(&m_keyboard, keymap);
      xkb_keymap_unref(keymap);
      xkb_context_unref(context);
      if (!applied) {
        throw std::runtime_error("could not apply the keyboard LED test keymap with a renamed indicator");
      }
    }

    void key(uint32_t code, wl_keyboard_key_state state) {
      wlr_keyboard_key_event event{.time_msec = ++m_time, .keycode = code, .update_state = true, .state = state};
      wlr_keyboard_notify_key(&m_keyboard, &event);
    }

    void tap(uint32_t code) {
      key(code, WL_KEYBOARD_KEY_STATE_PRESSED);
      key(code, WL_KEYBOARD_KEY_STATE_RELEASED);
    }

    void modifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
      wlr_keyboard_notify_modifiers(&m_keyboard, depressed, latched, locked, group);
    }

    [[nodiscard]] bool locked(const char* name) const {
      return xkb_state_mod_name_is_active(m_keyboard.xkb_state, name, XKB_STATE_MODS_LOCKED) > 0;
    }

    [[nodiscard]] xkb_keysym_t keysym(uint32_t code) const {
      return xkb_state_key_get_one_sym(m_keyboard.xkb_state, code + 8);
    }

    [[nodiscard]] uint32_t modifierMask(const char* name) const {
      const xkb_mod_index_t index = xkb_keymap_mod_get_index(m_keyboard.keymap, name);
      if (index == XKB_MOD_INVALID || index >= 32) {
        throw std::runtime_error("keyboard LED test keymap lacks the requested modifier");
      }
      return uint32_t{1} << index;
    }

    void finish() {
      if (m_finished) {
        return;
      }
      wl_list_remove(&m_key.link);
      wl_list_remove(&m_modifiers.link);
      wlr_keyboard_finish(&m_keyboard);
      m_finished = true;
    }

  private:
    static void recordLeds(wlr_keyboard* keyboard, uint32_t leds) {
      static_cast<BackendKeyboard*>(keyboard->data)->m_writes.push_back(leds);
    }

    static void onKey(wl_listener* listener, void* /*data*/) {
      BackendKeyboard* self;
      self = wl_container_of(listener, self, m_key);
      self->update();
    }

    static void onModifiers(wl_listener* listener, void* /*data*/) {
      BackendKeyboard* self;
      self = wl_container_of(listener, self, m_modifiers);
      self->update();
    }

    void update() {
      if (m_leds != nullptr && !m_leds->syncing()) {
        m_leds->update(m_changesSource ? &m_keyboard : nullptr);
      }
    }

    static constexpr wlr_keyboard_impl kWithLeds{.name = "recording LED backend", .led_update = recordLeds};
    static constexpr wlr_keyboard_impl kWithoutLeds{.name = "LED-less uinput backend", .led_update = nullptr};

    wlr_keyboard m_keyboard{};
    std::vector<uint32_t> m_writes;
    umbriel::KeyboardLeds* m_leds = nullptr;
    wl_listener m_key{};
    wl_listener m_modifiers{};
    uint32_t m_time = 0;
    bool m_changesSource = true;
    bool m_finished = false;
  };

} // namespace

UMBRIEL_TEST(ledlessSourceSharesTheFirstCapsAndNumTransitions) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard receiver;
  BackendKeyboard proxy(false);
  laptop.observe(leds);
  receiver.observe(leds);
  proxy.observe(leds);
  loop.flush();

  proxy.tap(58);
  loop.flush();
  CHECK_EQ(laptop.writes(), std::vector<uint32_t>{WLR_LED_CAPS_LOCK});
  CHECK_EQ(receiver.writes(), std::vector<uint32_t>{WLR_LED_CAPS_LOCK});
  CHECK(proxy.locked(XKB_MOD_NAME_CAPS));
  CHECK(laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK(receiver.locked(XKB_MOD_NAME_CAPS));

  receiver.tap(58);
  loop.flush();
  const std::vector<uint32_t> capsTransitions{WLR_LED_CAPS_LOCK, 0};
  CHECK_EQ(laptop.writes(), capsTransitions);
  CHECK_EQ(receiver.writes(), capsTransitions);
  CHECK(!proxy.locked(XKB_MOD_NAME_CAPS));
  CHECK(!laptop.locked(XKB_MOD_NAME_CAPS));

  proxy.tap(69);
  loop.flush();
  const std::vector<uint32_t> allTransitions{WLR_LED_CAPS_LOCK, 0, WLR_LED_NUM_LOCK};
  CHECK_EQ(laptop.writes(), allTransitions);
  CHECK_EQ(receiver.writes(), allTransitions);
  CHECK(proxy.locked(XKB_MOD_NAME_NUM));
  CHECK(laptop.locked(XKB_MOD_NAME_NUM));
  CHECK(receiver.locked(XKB_MOD_NAME_NUM));
}

UMBRIEL_TEST(alternatingOrdinaryInputPreservesNumLockWithoutLedFlicker) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard proxy(false);
  laptop.tap(69);
  laptop.observe(leds);
  proxy.observe(leds);
  loop.flush();
  CHECK(proxy.locked(XKB_MOD_NAME_NUM));
  laptop.clearWrites();

  for (BackendKeyboard* keyboard : {&laptop, &proxy, &laptop, &proxy}) {
    CHECK_EQ(keyboard->keysym(79), xkb_keysym_t{XKB_KEY_KP_1});
    keyboard->tap(30);
    keyboard->tap(79);
    loop.flush();
    CHECK(laptop.locked(XKB_MOD_NAME_NUM));
    CHECK(proxy.locked(XKB_MOD_NAME_NUM));
    CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_NUM_LOCK});
    CHECK(laptop.writes().empty());
  }

  proxy.tap(69);
  loop.flush();
  CHECK(!laptop.locked(XKB_MOD_NAME_NUM));
  CHECK(!proxy.locked(XKB_MOD_NAME_NUM));
  CHECK_EQ(laptop.keysym(79), xkb_keysym_t{XKB_KEY_KP_End});
  CHECK_EQ(laptop.writes(), std::vector<uint32_t>{0});
  laptop.tap(30);
  loop.flush();
  CHECK_EQ(laptop.writes(), std::vector<uint32_t>{0});
}

UMBRIEL_TEST(sharedLocksPreserveHeldModifiersLatchesLayoutsAndKeymaps) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop(true, "de,us");
  BackendKeyboard proxy(false, "us,de");
  const uint32_t shift = laptop.modifierMask(XKB_MOD_NAME_SHIFT);
  const uint32_t num = laptop.modifierMask(XKB_MOD_NAME_NUM);
  laptop.key(42, WL_KEYBOARD_KEY_STATE_PRESSED);
  laptop.modifiers(shift, shift, num, 1);
  laptop.observe(leds);
  proxy.observe(leds);
  loop.flush();
  laptop.clearWrites();
  xkb_keymap* originalMap = laptop.get()->keymap;

  proxy.tap(58);
  loop.flush();
  CHECK_EQ(laptop.writes(), std::vector<uint32_t>{WLR_LED_CAPS_LOCK | WLR_LED_NUM_LOCK});
  CHECK_EQ(laptop.get()->keymap, originalMap);
  CHECK_EQ(laptop.get()->modifiers.depressed, shift);
  CHECK_EQ(laptop.get()->modifiers.latched, shift);
  CHECK(laptop.locked(XKB_MOD_NAME_NUM));
  CHECK(laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK_EQ(laptop.get()->modifiers.group, uint32_t{1});
  CHECK_EQ(proxy.get()->modifiers.group, uint32_t{0});

  laptop.key(42, WL_KEYBOARD_KEY_STATE_RELEASED);
  loop.flush();
  CHECK_EQ(laptop.get()->modifiers.depressed, uint32_t{0});
  CHECK_EQ(laptop.get()->modifiers.group, uint32_t{1});
  CHECK(proxy.locked(XKB_MOD_NAME_NUM));
  CHECK(proxy.locked(XKB_MOD_NAME_CAPS));
}

UMBRIEL_TEST(hotplugAndKeymapReplacementInheritCurrentLocks) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard proxy(false);
  laptop.observe(leds);
  proxy.observe(leds);
  loop.flush();
  proxy.tap(58);
  loop.flush();

  BackendKeyboard hotplug;
  hotplug.tap(69);
  hotplug.observe(leds);
  loop.flush();
  CHECK(hotplug.locked(XKB_MOD_NAME_CAPS));
  CHECK(!hotplug.locked(XKB_MOD_NAME_NUM));
  CHECK_EQ(hotplug.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});

  laptop.setKeymap("de");
  loop.flush();
  CHECK(laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});
  CHECK_EQ(hotplug.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});

  proxy.setKeymap("fr");
  loop.flush();
  CHECK(proxy.locked(XKB_MOD_NAME_CAPS));
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});
  CHECK_EQ(hotplug.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});
  hotplug.tap(58);
  loop.flush();
  CHECK(!laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK(!proxy.locked(XKB_MOD_NAME_CAPS));
}

UMBRIEL_TEST(aReplacementKeymapGainsSharedLocksThatTheOldMapCannotRepresent) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard secondary;
  secondary.renameNumModifier();
  CHECK_EQ(xkb_keymap_mod_get_mask(secondary.get()->keymap, "NumLock"), xkb_mod_mask_t{0});
  laptop.tap(69);
  laptop.observe(leds);
  secondary.observe(leds);
  loop.flush();
  CHECK(laptop.locked(XKB_MOD_NAME_NUM));
  CHECK(!secondary.locked(XKB_MOD_NAME_NUM));

  secondary.setKeymap("us,de", true);
  loop.flush();
  CHECK(secondary.locked(XKB_MOD_NAME_NUM));
  CHECK(laptop.locked(XKB_MOD_NAME_NUM));
  CHECK_EQ(secondary.keysym(79), xkb_keysym_t{XKB_KEY_KP_1});
  CHECK_EQ(secondary.get()->leds, uint32_t{WLR_LED_NUM_LOCK});
  secondary.tap(30);
  loop.flush();
  CHECK(laptop.locked(XKB_MOD_NAME_NUM));
}

UMBRIEL_TEST(numLockStartupPolicyDoesNotOverrideLaterHotplugOrReconnects) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  laptop.observe(leds, true, false, true);
  loop.flush();
  CHECK(laptop.locked(XKB_MOD_NAME_NUM));
  CHECK_EQ(laptop.writes(), std::vector<uint32_t>{WLR_LED_NUM_LOCK});
  laptop.tap(69);
  loop.flush();
  CHECK(!laptop.locked(XKB_MOD_NAME_NUM));
  laptop.clearWrites();

  BackendKeyboard hotplug;
  hotplug.observe(leds, true, false, true);
  loop.flush();
  CHECK(!hotplug.locked(XKB_MOD_NAME_NUM));
  CHECK(!laptop.locked(XKB_MOD_NAME_NUM));
  CHECK(hotplug.writes().empty());
  CHECK(laptop.writes().empty());
  hotplug.finish();
  laptop.finish();
  loop.flush();

  BackendKeyboard reconnect;
  reconnect.observe(leds, true, false, true);
  loop.flush();
  CHECK(!reconnect.locked(XKB_MOD_NAME_NUM));
  CHECK(reconnect.writes().empty());
}

UMBRIEL_TEST(sourceRemovalBeforeIdlePreservesSharedLocks) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard proxy(false);
  laptop.observe(leds);
  proxy.observe(leds);
  loop.flush();
  proxy.tap(58);
  proxy.finish();
  loop.flush();
  CHECK(laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});

  BackendKeyboard replacement(false);
  replacement.observe(leds);
  CHECK(replacement.locked(XKB_MOD_NAME_CAPS));
  replacement.tap(69);
  loop.flush();
  const uint32_t both = WLR_LED_CAPS_LOCK | WLR_LED_NUM_LOCK;
  CHECK_EQ(laptop.get()->leds, both);
  replacement.finish();
  loop.flush();
  CHECK(laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK(laptop.locked(XKB_MOD_NAME_NUM));
  CHECK_EQ(laptop.get()->leds, both);
}

UMBRIEL_TEST(staleVirtualSnapshotsOnlyChangeLocksTheyActuallyToggle) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard client;
  laptop.tap(69);
  laptop.observe(leds);
  client.observe(leds, true, true);
  loop.flush();
  xkb_keymap* originalMap = client.get()->keymap;
  CHECK(!client.locked(XKB_MOD_NAME_NUM));
  laptop.clearWrites();
  client.clearWrites();

  const uint32_t caps = client.modifierMask(XKB_MOD_NAME_CAPS);
  const uint32_t shift = client.modifierMask(XKB_MOD_NAME_SHIFT);
  client.modifiers(0, 0, caps, 0);
  loop.flush();
  const uint32_t both = WLR_LED_CAPS_LOCK | WLR_LED_NUM_LOCK;
  CHECK(laptop.locked(XKB_MOD_NAME_NUM));
  CHECK(laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK_EQ(laptop.get()->leds, both);
  CHECK(!client.locked(XKB_MOD_NAME_NUM));
  CHECK_EQ(client.get()->keymap, originalMap);
  CHECK_EQ(client.writes(), std::vector<uint32_t>{WLR_LED_CAPS_LOCK});

  laptop.clearWrites();
  client.modifiers(shift, 0, caps, 0);
  client.tap(30);
  loop.flush();
  CHECK(laptop.locked(XKB_MOD_NAME_NUM));
  CHECK(laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK_EQ(laptop.get()->leds, both);
  CHECK(laptop.writes().empty());

  client.modifiers(0, 0, 0, 0);
  loop.flush();
  CHECK(!laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK(laptop.locked(XKB_MOD_NAME_NUM));
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_NUM_LOCK});

  laptop.tap(58);
  loop.flush();
  laptop.clearWrites();
  client.tap(30);
  loop.flush();
  CHECK(laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK(laptop.locked(XKB_MOD_NAME_NUM));
  CHECK_EQ(laptop.get()->leds, both);
  CHECK(laptop.writes().empty());
}

UMBRIEL_TEST(changedIndicatorNamesDoNotTurnInvalidIndexesIntoActiveLeds) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard proxy(false);
  laptop.observe(leds);
  proxy.observe(leds);
  loop.flush();
  proxy.tap(58);
  loop.flush();
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});

  proxy.renameCapsIndicator();
  proxy.tap(30);
  loop.flush();
  CHECK_EQ(xkb_keymap_led_get_index(proxy.get()->keymap, XKB_LED_NAME_CAPS), XKB_LED_INVALID);
  CHECK(proxy.locked(XKB_MOD_NAME_CAPS));
  CHECK_EQ(laptop.get()->leds, uint32_t{0});
  laptop.tap(30);
  loop.flush();
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});
}

UMBRIEL_TEST(losingTheFinalUsableSourceClearsSurvivingIndicators) {
  EventLoop loop;
  umbriel::KeyboardLeds leds(loop.get());
  BackendKeyboard laptop;
  BackendKeyboard proxy(false);
  laptop.observe(leds);
  proxy.observe(leds);
  loop.flush();
  proxy.tap(58);
  loop.flush();
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});

  CHECK(wlr_keyboard_set_keymap(laptop.get(), nullptr));
  proxy.finish();
  loop.flush();
  CHECK_EQ(laptop.get()->leds, uint32_t{0});
}

UMBRIEL_TEST(inputMethodEchoesCannotChangeSharedLocksOrVirtualKeymaps) {
  EventLoop loop;
  wlr_keyboard* excluded = nullptr;
  umbriel::KeyboardLeds leds(loop.get(), [&excluded](wlr_keyboard* source) { return source != excluded; });
  BackendKeyboard laptop;
  BackendKeyboard proxy(false);
  BackendKeyboard inputMethod;
  laptop.observe(leds);
  proxy.observe(leds);
  inputMethod.observe(leds, true, true);
  loop.flush();
  proxy.tap(58);
  loop.flush();
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});
  xkb_keymap* originalMap = inputMethod.get()->keymap;
  inputMethod.clearWrites();

  excluded = inputMethod.get();
  const uint32_t num = inputMethod.modifierMask(XKB_MOD_NAME_NUM);
  inputMethod.modifiers(0, 0, num, 0);
  loop.flush();
  CHECK(laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK(!laptop.locked(XKB_MOD_NAME_NUM));
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});
  CHECK_EQ(inputMethod.get()->keymap, originalMap);
  CHECK(!inputMethod.locked(XKB_MOD_NAME_CAPS));
  CHECK(inputMethod.locked(XKB_MOD_NAME_NUM));
  CHECK_EQ(inputMethod.writes(), std::vector<uint32_t>{WLR_LED_NUM_LOCK});

  excluded = nullptr;
  inputMethod.tap(30);
  loop.flush();
  CHECK(laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK(!laptop.locked(XKB_MOD_NAME_NUM));
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});

  leds.update(inputMethod.get());
  excluded = inputMethod.get();
  loop.flush();
  CHECK(laptop.locked(XKB_MOD_NAME_CAPS));
  CHECK(!laptop.locked(XKB_MOD_NAME_NUM));
  CHECK_EQ(laptop.get()->leds, uint32_t{WLR_LED_CAPS_LOCK});
}

UMBRIEL_TEST(destroyingTheRelayCancelsPendingCallbacksAndRemovesListeners) {
  EventLoop loop;
  BackendKeyboard keyboard;
  {
    umbriel::KeyboardLeds leds(loop.get());
    keyboard.observe(leds);
  }
  keyboard.stopObserving();
  // These emit registered signals after the relay has gone away. No stale
  // keymap/destroy listener or pending idle may access the destroyed object.
  keyboard.setKeymap("de");
  keyboard.finish();
  loop.flush();
  CHECK(keyboard.writes().empty());
}

int main() { return RUN_TESTS(); }
