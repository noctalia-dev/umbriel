#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace umbriel {

  struct TabSlot {
    int x = 0;
    int width = 0;
    bool operator==(const TabSlot&) const = default;
  };

  // The span of tab `index` among `count` equal tabs across `width` logical pixels, `gap` pixels apart. The division
  // remainder goes one pixel each to the leading tabs, so the slots and gaps tile the bar exactly. Drawing, hit
  // testing, and drop placement all come through here.
  constexpr TabSlot tabSlot(int width, size_t count, size_t index, int gap = 0) {
    if (count == 0 || index >= count || width <= 0) {
      return {};
    }
    const int tabs = static_cast<int>(count);
    const int at = static_cast<int>(index);
    // A bar too narrow for its gaps drops them rather than its tabs.
    const int spacing = std::max(0, gap) * (tabs - 1) < width ? std::max(0, gap) : 0;
    const int available = width - spacing * (tabs - 1);
    const int base = available / tabs;
    const int remainder = available % tabs;
    return {.x = at * (base + spacing) + std::min(at, remainder), .width = base + (at < remainder ? 1 : 0)};
  }

  // The tab whose slot, or the gap after it, holds `x` measured from the bar's left edge; -1 outside the bar.
  constexpr int tabIndexAt(int width, size_t count, double x, int gap = 0) {
    if (count == 0 || width <= 0 || x < 0.0 || x >= static_cast<double>(width)) {
      return -1;
    }
    for (size_t index = 0; index + 1 < count; ++index) {
      const TabSlot next = tabSlot(width, count, index + 1, gap);
      if (x < static_cast<double>(next.x)) {
        return static_cast<int>(index);
      }
    }
    return static_cast<int>(count) - 1;
  }

  // The run of tabs a bar shows: `count` slots from tab `first`.
  struct TabStrip {
    size_t first = 0;
    size_t count = 0;
    bool operator==(const TabStrip&) const = default;
  };

  // The tabs a bar of at most `maxVisible` slots shows out of `tabs`, 0 meaning every tab. It starts from where the
  // strip was, `previousFirst`, and moves only as far as it must to keep tab `active` in view, so stepping through the
  // tabs scrolls one slot at a time instead of jumping.
  constexpr TabStrip tabStrip(size_t tabs, size_t active, size_t maxVisible, size_t previousFirst) {
    if (maxVisible == 0 || tabs <= maxVisible) {
      return {.first = 0, .count = tabs};
    }
    size_t first = std::min(previousFirst, tabs - maxVisible);
    active = std::min(active, tabs - 1);
    if (active < first) {
      first = active;
    } else if (active >= first + maxVisible) {
      first = active + 1 - maxVisible;
    }
    return {.first = first, .count = maxVisible};
  }

  // Where a bar's parts sit across its width: a button at each end that cycles through every tab while it shows only
  // some of them, and the slots between them.
  struct TabBarParts {
    // 0 when the bar shows every tab and has no buttons.
    int buttonWidth = 0;
    int slotsX = 0;
    int slotsWidth = 0;
    bool operator==(const TabBarParts&) const = default;
  };

  // A cycle button is as long as the bar is thick, so it is square, but never shorter than a pointer can hit
  // comfortably, nor longer than a wide side bar needs, nor more than a quarter of the bar.
  inline constexpr int kTabCycleButtonMinWidth = 16;
  inline constexpr int kTabCycleButtonMaxWidth = 32;

  constexpr TabBarParts tabBarParts(int width, int height, int gap, bool scrolls) {
    if (!scrolls || width <= 0) {
      return {.buttonWidth = 0, .slotsX = 0, .slotsWidth = std::max(0, width)};
    }
    const int button = std::min(std::clamp(height, kTabCycleButtonMinWidth, kTabCycleButtonMaxWidth), width / 4);
    const int spacing = std::max(0, gap);
    return {
        .buttonWidth = button,
        .slotsX = button + spacing,
        .slotsWidth = std::max(1, width - 2 * (button + spacing)),
    };
  }

  enum class TabBarPart : uint8_t {
    None,
    Tab,
    PreviousTab,
    NextTab,
  };

  struct TabBarPoint {
    TabBarPart part = TabBarPart::None;
    // The tab under the point, counted among every tab, for TabBarPart::Tab.
    size_t index = 0;
    bool operator==(const TabBarPoint&) const = default;
  };

  // What lies at `x`, measured from the left edge of a bar `width` by `height` showing `strip` out of `tabs`.
  constexpr TabBarPoint tabBarPointAt(int width, int height, int gap, TabStrip strip, size_t tabs, double x) {
    if (width <= 0 || x < 0.0 || x >= static_cast<double>(width) || strip.count == 0) {
      return {};
    }
    const TabBarParts parts = tabBarParts(width, height, gap, strip.count < tabs);
    if (parts.buttonWidth > 0 && x < static_cast<double>(parts.buttonWidth)) {
      return {.part = TabBarPart::PreviousTab, .index = 0};
    }
    if (parts.buttonWidth > 0 && x >= static_cast<double>(width - parts.buttonWidth)) {
      return {.part = TabBarPart::NextTab, .index = 0};
    }
    const int slot = tabIndexAt(parts.slotsWidth, strip.count, x - parts.slotsX, gap);
    if (slot < 0) {
      return {};
    }
    return {.part = TabBarPart::Tab, .index = strip.first + static_cast<size_t>(slot)};
  }

  // Where a window dropped at `x` joins the tabs: before the tab under it when it lands on that tab's leading half,
  // after it otherwise. Returns a row in [0, count].
  constexpr size_t tabInsertionAt(int width, size_t count, double x, int gap = 0) {
    const int index = tabIndexAt(width, count, std::clamp(x, 0.0, static_cast<double>(std::max(0, width - 1))), gap);
    if (index < 0) {
      return count;
    }
    const TabSlot slot = tabSlot(width, count, static_cast<size_t>(index), gap);
    const bool leading = x < static_cast<double>(slot.x) + static_cast<double>(slot.width) / 2.0;
    return static_cast<size_t>(index) + (leading ? 0 : 1);
  }

  // Where a window dropped at `x` on such a bar joins the tabs, and the boundary between slots that shows it, both
  // measured as tabBarPointAt measures.
  struct TabDropPoint {
    size_t row = 0;
    int boundary = 0;
    bool operator==(const TabDropPoint&) const = default;
  };

  constexpr TabDropPoint tabBarDropAt(int width, int height, int gap, TabStrip strip, size_t tabs, double x) {
    const TabBarParts parts = tabBarParts(width, height, gap, strip.count < tabs);
    const size_t position = tabInsertionAt(parts.slotsWidth, strip.count, x - parts.slotsX, gap);
    const int boundary = position == 0 ? 0
        : position >= strip.count      ? parts.slotsWidth
                                       : tabSlot(parts.slotsWidth, strip.count, position, gap).x - gap / 2;
    return {.row = std::min(strip.first + position, tabs), .boundary = parts.slotsX + boundary};
  }

} // namespace umbriel
