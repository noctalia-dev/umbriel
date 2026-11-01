#include "input/keyboard_leds.h"

#include "input/keyboard_state.h"

#include <algorithm>
#include <cstdint>
#include <utility>

extern "C" {
#include <wlr/types/wlr_keyboard.h>
}

namespace umbriel {

  KeyboardLeds::KeyboardLeds(wl_event_loop* loop, std::function<bool(wlr_keyboard*)> canUseSource)
      : m_loop(loop), m_canUseSource(std::move(canUseSource)) {}

  KeyboardLeds::~KeyboardLeds() {
    if (m_idle != nullptr) {
      wl_event_source_remove(m_idle);
    }
    for (Entry& entry : m_entries) {
      wl_list_remove(&entry.keymap.link);
      wl_list_remove(&entry.destroy.link);
      xkb_state_unref(entry.indicators);
    }
  }

  bool KeyboardLeds::usableSource(wlr_keyboard* keyboard) const {
    return keyboard != nullptr
        && std::ranges::any_of(m_entries, [keyboard](const Entry& entry) { return entry.keyboard == keyboard; })
        && keyboard->xkb_state != nullptr
        && (!m_canUseSource || m_canUseSource(keyboard));
  }

  wlr_keyboard* KeyboardLeds::fallbackSource() const {
    for (const Entry& entry : m_entries) {
      if (!entry.virtualDevice && usableSource(entry.keyboard)) {
        return entry.keyboard;
      }
    }
    return nullptr;
  }

  void KeyboardLeds::add(wlr_keyboard* keyboard, bool virtualDevice, bool initialNumLock) {
    if (keyboard == nullptr
        || std::ranges::any_of(m_entries, [keyboard](const Entry& entry) { return entry.keyboard == keyboard; })) {
      return;
    }
    Entry& entry = m_entries.emplace_back();
    entry.owner = this;
    entry.keyboard = keyboard;
    entry.virtualDevice = virtualDevice;
    entry.initialNumLock = initialNumLock;
    if (keyboard->keymap != nullptr) {
      entry.indicators = xkb_state_new(keyboard->keymap);
      entry.locked = keyboardModifierNames(keyboard->keymap, keyboard->modifiers.locked);
    }
    entry.keymap.notify = onKeymap;
    wl_signal_add(&keyboard->events.keymap, &entry.keymap);
    entry.destroy.notify = onDestroy;
    wl_signal_add(&keyboard->base.events.destroy, &entry.destroy);
    if (m_source == nullptr && !virtualDevice && usableSource(keyboard)) {
      m_source = keyboard;
    }
    if (!m_initialized && !virtualDevice && usableSource(keyboard)) {
      initializeLocks(entry);
    }
    update();
  }

  void KeyboardLeds::remove(wlr_keyboard* keyboard) {
    const auto entry =
        std::ranges::find_if(m_entries, [keyboard](const Entry& candidate) { return candidate.keyboard == keyboard; });
    if (entry == m_entries.end()) {
      return;
    }
    wl_list_remove(&entry->keymap.link);
    wl_list_remove(&entry->destroy.link);
    xkb_state_unref(entry->indicators);
    m_entries.erase(entry);
    if (m_source == keyboard) {
      m_source = fallbackSource();
    }
    update();
  }

  void KeyboardLeds::update(wlr_keyboard* source) {
    if (!m_syncing) {
      for (Entry& entry : m_entries) {
        if (entry.keyboard == source && usableSource(source)) {
          m_source = source;
          observeLocks(entry);
        } else if (!usableSource(entry.keyboard) && entry.keyboard->keymap != nullptr) {
          // Ownership can change after a virtual keyboard is registered. Keep
          // excluded echoes from becoming apparent lock changes when it returns.
          entry.locked = keyboardModifierNames(entry.keyboard->keymap, entry.keyboard->modifiers.locked);
        }
      }
      syncPhysicalLocks();
    }
    if (m_idle == nullptr) {
      m_idle = wl_event_loop_add_idle(m_loop, onIdle, this);
    }
  }

  void KeyboardLeds::onKeymap(wl_listener* listener, void* /*data*/) {
    Entry* entry;
    entry = wl_container_of(listener, entry, keymap);
    xkb_state_unref(entry->indicators);
    entry->indicators = entry->keyboard->keymap != nullptr ? xkb_state_new(entry->keyboard->keymap) : nullptr;
    entry->locked = entry->keyboard->keymap != nullptr
        ? keyboardModifierNames(entry->keyboard->keymap, entry->keyboard->modifiers.locked)
        : std::vector<std::string>{};
    if (!entry->owner->m_initialized && !entry->virtualDevice && entry->owner->usableSource(entry->keyboard)) {
      entry->owner->initializeLocks(*entry);
    }
    entry->owner->update();
  }

  void KeyboardLeds::initializeLocks(const Entry& entry) {
    m_locked = entry.locked;
    if (entry.initialNumLock) {
      const char* name =
          xkb_keymap_mod_get_index(entry.keyboard->keymap, "NumLock") != XKB_MOD_INVALID ? "NumLock" : XKB_MOD_NAME_NUM;
      if (std::ranges::find(m_locked, name) == m_locked.end()) {
        m_locked.emplace_back(name);
      }
    }
    m_initialized = true;
  }

  void KeyboardLeds::observeLocks(Entry& entry) {
    const auto current = keyboardModifierNames(entry.keyboard->keymap, entry.keyboard->modifiers.locked);
    if (!m_initialized) {
      m_locked = current;
      m_initialized = true;
    } else {
      for (const std::string& name : entry.locked) {
        if (std::ranges::find(current, name) == current.end()) {
          std::erase(m_locked, name);
        }
      }
      for (const std::string& name : current) {
        if (std::ranges::find(entry.locked, name) == entry.locked.end()
            && std::ranges::find(m_locked, name) == m_locked.end()) {
          m_locked.push_back(name);
        }
      }
    }
    entry.locked = current;
  }

  void KeyboardLeds::syncPhysicalLocks() {
    if (!m_initialized) {
      return;
    }
    m_syncing = true;
    for (Entry& entry : m_entries) {
      if (entry.virtualDevice || !usableSource(entry.keyboard)) {
        continue;
      }
      const auto locked = keyboardModifierMask(entry.keyboard->keymap, m_locked);
      entry.locked = keyboardModifierNames(entry.keyboard->keymap, locked);
      if (entry.keyboard->modifiers.locked != locked) {
        setKeyboardLatchedLockedModifiers(entry.keyboard, entry.keyboard->modifiers.latched, locked);
      }
      entry.locked = keyboardModifierNames(entry.keyboard->keymap, entry.keyboard->modifiers.locked);
    }
    m_syncing = false;
  }

  void KeyboardLeds::onDestroy(wl_listener* listener, void* /*data*/) {
    Entry* entry;
    entry = wl_container_of(listener, entry, destroy);
    entry->owner->remove(entry->keyboard);
  }

  void KeyboardLeds::onIdle(void* data) {
    auto* self = static_cast<KeyboardLeds*>(data);
    self->m_idle = nullptr;
    self->flush();
  }

  void KeyboardLeds::flush() {
    if (!usableSource(m_source)) {
      m_source = fallbackSource();
    }
    // A virtual source may carry stale locks other than the one it changed.
    // Evaluate its indicator mapping with shared locks without editing its state.
    uint32_t leds = 0;
    const auto source = std::ranges::find_if(m_entries, [this](const Entry& entry) {
      return entry.keyboard == m_source && entry.indicators != nullptr;
    });
    if (source != m_entries.end()) {
      xkb_state* state = m_source->xkb_state;
      xkb_state_update_mask(
          source->indicators, m_source->modifiers.depressed, m_source->modifiers.latched,
          keyboardModifierMask(m_source->keymap, m_locked),
          xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_DEPRESSED),
          xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_LATCHED),
          xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_LOCKED)
      );
      for (uint32_t i = 0; i < WLR_LED_COUNT; ++i) {
        const xkb_led_index_t index = m_source->led_indexes[i];
        if (index != XKB_LED_INVALID && xkb_state_led_index_is_active(source->indicators, index) > 0) {
          leds |= uint32_t{1} << i;
        }
      }
    }
    for (const Entry& entry : m_entries) {
      if (!entry.virtualDevice) {
        wlr_keyboard_led_update(entry.keyboard, leds);
      }
    }
  }

} // namespace umbriel
