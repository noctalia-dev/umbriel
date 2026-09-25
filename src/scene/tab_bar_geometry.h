#pragma once

#include <algorithm>
#include <cstddef>

namespace umbriel {

  struct TabSlot {
    int x = 0;
    int width = 0;
  };

  // The span of tab `index` among `count` equal tabs across `width` logical pixels. The division remainder goes one
  // pixel each to the leading tabs, so the slots tile the bar exactly. Drawing and hit testing both come through here.
  [[nodiscard]] constexpr TabSlot tabSlot(int width, size_t count, size_t index) {
    if (count == 0 || index >= count || width <= 0) {
      return {};
    }
    const int tabs = static_cast<int>(count);
    const int at = static_cast<int>(index);
    const int base = width / tabs;
    const int remainder = width % tabs;
    return {.x = at * base + std::min(at, remainder), .width = base + (at < remainder ? 1 : 0)};
  }

  // The tab under `x`, measured from the bar's left edge, or -1 outside the bar.
  [[nodiscard]] constexpr int tabIndexAt(int width, size_t count, double x) {
    if (count == 0 || width <= 0 || x < 0.0 || x >= static_cast<double>(width)) {
      return -1;
    }
    for (size_t index = 0; index < count; ++index) {
      const TabSlot slot = tabSlot(width, count, index);
      if (x < static_cast<double>(slot.x + slot.width)) {
        return static_cast<int>(index);
      }
    }
    return static_cast<int>(count) - 1;
  }

} // namespace umbriel
