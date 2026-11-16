#pragma once

#include "input/global_shortcuts.h"

#include <functional>
#include <memory>
#include <string_view>

struct wl_display;

namespace umbriel {

  class GlobalShortcutsStore;

  class GlobalShortcutsBridge {
  public:
    enum class Authorization : uint8_t { Approved, Cancelled, Failed };
    using AuthorizationRequest =
        std::function<void(GlobalShortcutSessionId, std::string_view, const GlobalShortcutBind&)>;
    using AuthorizationCancelled = std::function<void(GlobalShortcutSessionId)>;

    // The display, registry, and store must outlive the bridge.
    GlobalShortcutsBridge(wl_display* display, GlobalShortcutsRegistry& registry, GlobalShortcutsStore& store);
    ~GlobalShortcutsBridge();
    GlobalShortcutsBridge(const GlobalShortcutsBridge&) = delete;
    GlobalShortcutsBridge& operator=(const GlobalShortcutsBridge&) = delete;

    void setAuthorize(AuthorizationRequest request);
    void setAuthorizationCancelled(AuthorizationCancelled cancelled);
    // Only the compositor's physical approval path may supply Approved.
    void authorizeSession(GlobalShortcutSessionId id, Authorization authorization);
    // Flush registry changes after configuration or revocation.
    void dispatchChanges();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
