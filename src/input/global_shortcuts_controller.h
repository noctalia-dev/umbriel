#pragma once

#include <cstdint>
#include <memory>
#include <span>
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

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
