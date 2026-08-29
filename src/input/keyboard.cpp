#include "input/keyboard.h"

#include "config/config.h"
#include "core/log.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "input/shortcut_keysym.h"
#include "input/text_input.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "scene/quit_confirm.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>
#include <optional>

namespace umbriel {

  namespace {
    constexpr Logger kLog("keyboard");
  } // namespace

  Keyboard::Keyboard(Server& server, wlr_input_device* device)
      : m_server(&server), m_keyboard(wlr_keyboard_from_input_device(device)),
        m_virtual(wlr_input_device_get_virtual_keyboard(device) != nullptr),
        m_deviceName(device->name != nullptr ? device->name : "") {
    applyConfig();
    if (!m_virtual && config().input.keyboard.numlockToggle && m_keyboard->keymap != nullptr) {
      const xkb_mod_index_t numLock = xkb_keymap_mod_get_index(m_keyboard->keymap, XKB_MOD_NAME_NUM);
      if (numLock != XKB_MOD_INVALID) {
        wlr_keyboard_notify_modifiers(m_keyboard, 0, 0, 1U << numLock, 0);
      }
    }

    m_modifiers.notify = onModifiers;
    wl_signal_add(&m_keyboard->events.modifiers, &m_modifiers);
    m_key.notify = onKey;
    wl_signal_add(&m_keyboard->events.key, &m_key);
    m_destroy.notify = onDestroy;
    wl_signal_add(&device->events.destroy, &m_destroy);

    // A virtual keyboard is announced before its client necessarily provides a
    // keymap. Do not replace a usable keyboard already on the seat with that
    // incomplete device; its first modifiers or key event selects it below. An
    // empty seat still has to take it: keyboard focus enter and the
    // input-method grab keymap both read the seat's current keyboard, so a
    // session whose only keyboards are virtual would never focus or type.
    wlr_seat* seat = m_server->seat()->wlr();
    if (!m_virtual || wlr_seat_get_keyboard(seat) == nullptr) {
      wlr_seat_set_keyboard(seat, m_keyboard);
    }
  }
  void Keyboard::applyConfig() {
    cancelRepeat();
    if (m_virtual) {
      return;
    }
    const Config::Input& input = config().input;
    const Config::Input::Device* device = input.findDevice(m_deviceName);
    const std::string& layout = device != nullptr && device->layout ? *device->layout : input.keyboard.layout;
    const std::string& variant = device != nullptr && device->variant ? *device->variant : input.keyboard.variant;
    const std::string& options = device != nullptr && device->options ? *device->options : input.keyboard.options;
    const int repeatRate = device != nullptr && device->repeatRate ? *device->repeatRate : input.keyboard.repeatRate;
    const int repeatDelay =
        device != nullptr && device->repeatDelay ? *device->repeatDelay : input.keyboard.repeatDelay;
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    const xkb_rule_names names{
        .rules = nullptr,
        .model = nullptr,
        .layout = layout.empty() ? nullptr : layout.c_str(),
        .variant = variant.empty() ? nullptr : variant.c_str(),
        .options = options.empty() ? nullptr : options.c_str(),
    };
    xkb_keymap* keymap =
        context == nullptr ? nullptr : xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap != nullptr) {
      wlr_keyboard_set_keymap(m_keyboard, keymap);
      xkb_keymap_unref(keymap);
    } else {
      wlr_log(WLR_ERROR, "failed to create keyboard keymap");
    }
    if (context != nullptr) {
      xkb_context_unref(context);
    }
    wlr_keyboard_set_repeat_info(m_keyboard, repeatRate, repeatDelay);
    // The name list or keymap changed; resend the current state so consumers
    // observe the reload even when the effective group did not move.
    notifyLayoutIfChanged();
  }

  bool Keyboard::cycleLayout() {
    // A virtual keyboard's keymap belongs to its client, so its groups are not
    // ours to rotate. Same reason applyConfig leaves them alone.
    if (m_virtual || m_keyboard->keymap == nullptr || m_keyboard->xkb_state == nullptr) {
      return false;
    }
    const xkb_layout_index_t count = xkb_keymap_num_layouts(m_keyboard->keymap);
    if (count < 2) {
      return false;
    }
    const xkb_layout_index_t current = xkb_state_serialize_layout(m_keyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    const xkb_layout_index_t next = (current + 1) % count;
    // Locking the group through wlroots rather than xkbcommon directly is what gets the new group out to clients: it
    // refreshes wlr_keyboard::modifiers and emits the modifiers event the seat forwards.
    wlr_keyboard_notify_modifiers(
        m_keyboard, m_keyboard->modifiers.depressed, m_keyboard->modifiers.latched, m_keyboard->modifiers.locked, next
    );
    const xkb_layout_index_t effective = xkb_state_serialize_layout(m_keyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    const char* name = xkb_keymap_layout_get_name(m_keyboard->keymap, effective);
    kLog.info(
        "'{}' switched to layout {}/{} ({})", m_deviceName, effective + 1, count, name != nullptr ? name : "unnamed"
    );
    return effective == next;
  }

  void Keyboard::notifyLayoutIfChanged() {
    if (m_virtual || m_keyboard->keymap == nullptr || m_keyboard->xkb_state == nullptr) {
      return;
    }
    const xkb_layout_index_t effective = xkb_state_serialize_layout(m_keyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    if (effective == m_lastNotifiedLayout) {
      return;
    }
    m_lastNotifiedLayout = effective;
    m_server->notifyKeyboardLayoutIpc();
  }

  wlr_input_method_keyboard_grab_v2* Keyboard::activeInputMethodGrab() const {
    if (m_server->sessionLocked()) {
      return nullptr;
    }
    return m_server->inputMethodRelay()->grabForKeyboard(m_keyboard);
  }

  Keyboard::~Keyboard() {
    if (m_repeatTimer != nullptr) {
      wl_event_source_remove(m_repeatTimer);
      m_repeatTimer = nullptr;
    }
    if (m_modifiers.link.next != nullptr) {
      wl_list_remove(&m_modifiers.link);
      wl_list_remove(&m_key.link);
      wl_list_remove(&m_destroy.link);
    }
  }

  void Keyboard::onModifiers(wl_listener* listener, void* /*data*/) {
    Keyboard* self;
    self = wl_container_of(listener, self, m_modifiers);
    self->handleModifiers();
  }

  void Keyboard::onKey(wl_listener* listener, void* data) {
    Keyboard* self;
    self = wl_container_of(listener, self, m_key);
    self->handleKey(data);
  }

  void Keyboard::onDestroy(wl_listener* listener, void* /*data*/) {
    Keyboard* self;
    self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  void Keyboard::handleModifiers() {
    cancelRepeat();
    m_server->notifyIdleActivity();
    wlr_seat* seat = m_server->seat()->wlr();
    wlr_input_method_keyboard_grab_v2* grab = activeInputMethodGrab();
    if (grab != nullptr) {
      wlr_input_method_keyboard_grab_v2_set_keyboard(grab, m_keyboard);
      wlr_input_method_keyboard_grab_v2_send_modifiers(grab, &m_keyboard->modifiers);
    } else {
      wlr_seat_set_keyboard(seat, m_keyboard);
      wlr_seat_keyboard_notify_modifiers(seat, &m_keyboard->modifiers);
    }
    m_server->cursor()->refreshInteractiveCursor();
    notifyLayoutIfChanged();
  }

  void Keyboard::handleKey(void* data) {
    auto* event = static_cast<wlr_keyboard_key_event*>(data);
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
      m_server->notifyInputActivity();
    } else {
      m_server->notifyIdleActivity();
    }
    wlr_seat* seat = m_server->seat()->wlr();

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t* syms = nullptr;
    int nsyms = xkb_state_key_get_syms(m_keyboard->xkb_state, keycode, &syms);
    xkb_keymap* keymap = xkb_state_get_keymap(m_keyboard->xkb_state);
    const xkb_layout_index_t layout = xkb_state_key_get_layout(m_keyboard->xkb_state, keycode);
    const uint32_t rawSym = rawShortcutKeysym(keymap, keycode, layout);
    const bool modifierOnly = nsyms > 0 && syms[0] >= XKB_KEY_Shift_L && syms[0] <= XKB_KEY_Hyper_R;

    bool handled = false;
    std::optional<Keybind> modifierTap;
    // An XF86 key can arrive from a dedicated hotkey device while its modifiers are held on the main keyboard. Match
    // against the combined seat-wide state so those chords behave like keys on one device.
    uint32_t modifiers = m_server->keyboardModifiers();
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
      if (!modifierOnly && nsyms > 0) {
        m_server->cursor()->noteTyping();
      }
      m_server->armModifierTap(
          this, event->keycode, std::span<const uint32_t>(syms, nsyms > 0 ? static_cast<size_t>(nsyms) : 0), modifiers
      );
      cancelRepeat();
      for (int i = 0; i < nsyms; ++i) {
        handled = m_server->handleVtSwitch(syms[i], modifiers) || handled;
      }
      // Modal quit confirmation: Enter or the session-quit bind confirms, any other non-modifier key cancels. The press
      // is consumed either way, reaching neither binds nor clients. Modifier-only presses pass through so held chords
      // stay intact.
      bool quitConfirmConsumed = false;
      if (QuitConfirm* confirm = m_server->quitConfirm(); confirm != nullptr && confirm->visible()) {
        if (!modifierOnly) {
          bool confirmed = false;
          for (int i = 0; i < nsyms; ++i) {
            confirmed = confirmed || syms[i] == XKB_KEY_Return || syms[i] == XKB_KEY_KP_Enter;
          }
          // Pressing the quit bind again confirms. matchKeybind mirrors handleKeybind without running the action, so a
          // press that would fire session-quit quits; any other matched or unbound press dismisses.
          for (int i = 0; i < nsyms && !confirmed; ++i) {
            const Keybind* matched = m_server->matchKeybind(syms[i], rawSym, modifiers);
            if (matched != nullptr && matched->action == KeybindAction::SessionQuit) {
              confirmed = true;
            }
          }
          if (confirmed) {
            m_server->stop();
          } else {
            confirm->hide();
          }
          handled = true;
          quitConfirmConsumed = true;
        }
      }
      std::optional<Keybind> matched;
      if (!quitConfirmConsumed) {
        for (int i = 0; i < nsyms; ++i) {
          std::optional<Keybind> result = m_server->handleKeybind(syms[i], rawSym, modifiers);
          if (result.has_value()) {
            matched = std::move(result);
            handled = true;
            break;
          }
        }
      }
      if (matched.has_value()) {
        armRepeat(*matched, event->keycode);
      }
      // Unbound plain keys drive overview navigation instead of reaching clients, unless a layer surface (launcher,
      // panel) holds the keyboard: its Escape/arrows belong to it, not to the filmstrip.
      if (!handled && seat->keyboard_state.focused_surface == nullptr) {
        Overview* overview = m_server->overview();
        const uint32_t plain = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
        if (overview != nullptr && overview->interactive() && plain == 0) {
          for (int i = 0; i < nsyms && !handled; ++i) {
            handled = overview->handleFallbackKey(syms[i]);
          }
        }
      }
      // Any non-modifier key press dismisses the cheatsheet, except the key
      // that just toggled it.
      if (Cheatsheet* sheet = m_server->cheatsheet(); sheet != nullptr && sheet->visible()) {
        const bool cheatsheetBind = matched.has_value() && isCheatsheetAction(matched->action);
        if (!cheatsheetBind && !modifierOnly) {
          sheet->hide();
        }
      }
    } else if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
      if (m_repeatArmed && event->keycode == m_repeatKeycode) {
        cancelRepeat();
      }
      modifierTap = m_server->releaseModifierTap(this, event->keycode);
    }

    if (!handled) {
      wlr_input_method_keyboard_grab_v2* grab = activeInputMethodGrab();
      if (grab != nullptr) {
        wlr_input_method_keyboard_grab_v2_set_keyboard(grab, m_keyboard);
        wlr_input_method_keyboard_grab_v2_send_key(grab, event->time_msec, event->keycode, event->state);
      } else {
        // Overview holds the seat, so windows see no keys until it closes. It never hands keyboard focus to a view
        // while open (focusView skips the seat enter), so a non-null focus here is a layer surface that took it
        // deliberately, e.g. a launcher panel; those keep typing.
        const Overview* overview = m_server->overview();
        const bool overviewOwnsKeyboard =
            overview != nullptr && overview->active() && seat->keyboard_state.focused_surface == nullptr;
        if (!overviewOwnsKeyboard) {
          wlr_seat_set_keyboard(seat, m_keyboard);
          wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
        }
      }
    }
    // A modifier-only action runs after its release reaches the focused
    // client, so opening a surface cannot strand the old focus with Mod held.
    if (modifierTap.has_value()) {
      m_server->executeKeybindAction(*modifierTap);
    }
  }

  void Keyboard::handleDestroy() {
    if (m_repeatTimer != nullptr) {
      wl_event_source_remove(m_repeatTimer);
      m_repeatTimer = nullptr;
    }
    wl_list_remove(&m_modifiers.link);
    wl_list_remove(&m_key.link);
    wl_list_remove(&m_destroy.link);
    m_modifiers.link.next = nullptr;
    m_key.link.next = nullptr;
    m_destroy.link.next = nullptr;
    m_server->cancelModifierTap();
    m_server->removeKeyboard(this);
  }

  void Keyboard::armRepeat(const Keybind& bind, uint32_t keycode) {
    const int32_t rate = m_keyboard->repeat_info.rate;
    const int32_t delay = m_keyboard->repeat_info.delay;
    const bool repeatable =
        bind.action != KeybindAction::ScratchpadToggle && bind.action != KeybindAction::ScratchpadFocusNext;
    if (!bind.repeat || !repeatable || rate <= 0 || delay <= 0) {
      cancelRepeat();
      return;
    }
    m_repeatBind = bind;
    m_repeatKeycode = keycode;
    m_repeatIntervalMs = std::max(1, 1000 / rate);
    if (m_repeatTimer == nullptr) {
      wl_event_loop* loop = wl_display_get_event_loop(m_server->display());
      m_repeatTimer = wl_event_loop_add_timer(loop, onRepeatTimer, this);
      if (m_repeatTimer == nullptr) {
        return;
      }
    }
    wl_event_source_timer_update(m_repeatTimer, delay);
    m_repeatArmed = true;
  }

  void Keyboard::cancelRepeat() {
    if (m_repeatArmed) {
      wl_event_source_timer_update(m_repeatTimer, 0);
      m_repeatArmed = false;
    }
  }

  int Keyboard::onRepeatTimer(void* data) {
    auto* self = static_cast<Keyboard*>(data);
    if (!self->m_repeatArmed) {
      return 0;
    }
    if (self->m_server->sessionLocked() && !self->m_repeatBind.allowWhenLocked) {
      self->cancelRepeat();
      return 0;
    }
    self->m_server->notifyIdleActivity();
    self->m_server->executeKeybindAction(self->m_repeatBind);
    wl_event_source_timer_update(self->m_repeatTimer, self->m_repeatIntervalMs);
    return 0;
  }

} // namespace umbriel
