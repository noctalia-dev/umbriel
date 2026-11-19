#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <xkbcommon/xkbcommon.h>

namespace umbriel {

  struct GlobalShortcutTrigger {
    uint32_t modifiers = 0;
    xkb_keysym_t keysym = XKB_KEY_NoSymbol;

    bool operator==(const GlobalShortcutTrigger&) const = default;
  };

  std::optional<GlobalShortcutTrigger> parseGlobalShortcutTrigger(std::string_view text);
  bool preferredGlobalShortcutAllowed(const GlobalShortcutTrigger& trigger);
  std::string describeGlobalShortcutTrigger(const GlobalShortcutTrigger& trigger);

} // namespace umbriel
