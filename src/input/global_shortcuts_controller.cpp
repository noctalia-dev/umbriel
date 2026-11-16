#include "input/global_shortcuts_controller.h"

#include "config/config.h"
#include "core/log.h"
#include "input/global_shortcuts_bridge.h"
#include "input/global_shortcuts_input.h"
#include "input/global_shortcuts_store.h"
#include "input/input_capture.h"
#include "scene/quit_confirm.h"
#include "server/server.h"
#include "wlr.h"

#include <chrono>
#include <vector>

namespace umbriel {
  namespace {
    constexpr Logger kLog("global-shortcuts");

    uint32_t timeMsec() {
      return static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count()
      );
    }
  } // namespace

  struct GlobalShortcuts::Impl {
    Server& server;
    GlobalShortcutsRegistry registry;
    GlobalShortcutsStore store{GlobalShortcutsStore::defaultPath()};
    GlobalShortcutsBridge bridge;
    bool configured = false;

    explicit Impl(Server& server) : server(server), bridge(server.display(), registry, store) {}

    bool allowed() const {
      return configured
          && !server.stopping()
          && !server.sessionLocked()
          && server.sessionLock() == nullptr
          && (server.session() == nullptr || server.session()->active)
          && (server.inputCapture() == nullptr || !server.inputCapture()->active())
          && (server.quitConfirm() == nullptr || !server.quitConfirm()->visible())
          && !server.inSubmap()
          && !server.keyboardShortcutsInhibited();
    }
  };

  GlobalShortcuts::GlobalShortcuts(Server& server) : m_impl(std::make_unique<Impl>(server)) { refreshConfig(); }
  GlobalShortcuts::~GlobalShortcuts() = default;

  bool GlobalShortcuts::observeKey(const void* source, uint32_t keycode, uint32_t timeMsec, bool pressed) {
    return m_impl->bridge.input().observeKey(source, keycode, timeMsec, pressed);
  }

  bool GlobalShortcuts::handlePress(
      const void* source, uint32_t keycode, uint32_t timeMsec, std::span<const xkb_keysym_t> keysyms,
      xkb_keysym_t rawKeysym, uint32_t modifiers
  ) {
    return m_impl->allowed()
        && m_impl->bridge.input().handlePress(source, keycode, timeMsec, keysyms, rawKeysym, modifiers);
  }

  void GlobalShortcuts::forgetSource(const void* source) { m_impl->bridge.input().forgetSource(source, timeMsec()); }
  void GlobalShortcuts::suspend() { m_impl->bridge.input().suspend(timeMsec()); }

  void GlobalShortcuts::refreshConfig() {
    std::vector<GlobalShortcutTrigger> reserved;
    for (const auto& bind : config().keybinds) {
      if (bind.submap.empty() && !bind.modifierOnly && bind.mouseButton == 0 && bind.wheel == WheelDirection::None) {
        reserved.push_back({bind.modifiers | (bind.useMod ? m_impl->server.modKey() : 0), bind.keysym});
      }
    }
    m_impl->configured = m_impl->registry.configure({}, reserved, m_impl->server.modKey());
    if (!m_impl->configured) {
      suspend();
      kLog.error("could not reserve configured keybinds for global shortcuts");
    }
    m_impl->bridge.dispatchChanges();
  }

} // namespace umbriel
