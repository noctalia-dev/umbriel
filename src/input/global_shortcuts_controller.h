#pragma once

#include "input/global_shortcuts_bridge.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace umbriel {

  class Server;

  class GlobalShortcuts {
  public:
    explicit GlobalShortcuts(Server& server);
    ~GlobalShortcuts();
    GlobalShortcuts(const GlobalShortcuts&) = delete;
    GlobalShortcuts& operator=(const GlobalShortcuts&) = delete;

    bool observeKey(const void* source, uint32_t keycode, uint32_t timeMsec, bool pressed);
    bool handlePress(
        const void* source, uint32_t keycode, uint32_t timeMsec, std::span<const xkb_keysym_t> keysyms,
        xkb_keysym_t rawKeysym, uint32_t modifiers
    );
    void forgetSource(const void* source);
    void suspend();
    void refreshConfig();
    void setAuthorize(GlobalShortcutsBridge::AuthorizationRequest request);
    void setAuthorizationCancelled(GlobalShortcutsBridge::AuthorizationCancelled cancelled);
    void authorizeSession(GlobalShortcutSessionId id, GlobalShortcutsBridge::Authorization authorization);
    std::optional<std::vector<GlobalShortcutApplication>> inspect() const;
    [[nodiscard]] bool revoke(std::string_view application);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
