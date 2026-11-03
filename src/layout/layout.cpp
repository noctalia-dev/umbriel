#include "layout/layout.h"

#include "config/config.h"
#include "layout/dwindle.h"
#include "layout/master.h"
#include "layout/scrolling.h"
#include "layout/stacking.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <utility>

extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}

namespace umbriel {

  wlr_box applyLayoutStruts(const wlr_box& area, const LayoutStruts& struts) {
    const auto coordinate = [](int value, int offset) {
      return static_cast<int>(std::clamp(
          static_cast<int64_t>(value) + offset, static_cast<int64_t>(std::numeric_limits<int>::min()),
          static_cast<int64_t>(std::numeric_limits<int>::max())
      ));
    };
    const auto extent = [](int value, int start, int end) {
      return static_cast<int>(std::clamp(
          static_cast<int64_t>(value) - start - end, int64_t{0}, static_cast<int64_t>(std::numeric_limits<int>::max())
      ));
    };
    return {
        .x = coordinate(area.x, struts.left),
        .y = coordinate(area.y, struts.top),
        .width = extent(area.width, struts.left, struts.right),
        .height = extent(area.height, struts.top, struts.bottom),
    };
  }

  std::optional<std::vector<View*>> resolveLayoutMembers(size_t memberCount, std::span<const LayoutMember> members) {
    std::vector<View*> resolved(memberCount, nullptr);
    for (const LayoutMember& member : members) {
      const auto id = static_cast<size_t>(member.id);
      if (member.view == nullptr
          || id >= resolved.size()
          || resolved[id] != nullptr
          || std::ranges::find(resolved, member.view) != resolved.end()) {
        return std::nullopt;
      }
      resolved[id] = member.view;
    }
    return resolved;
  }

  wlr_box Layout::contentArea(const wlr_box& usable) const {
    const int edgePad = m_config != nullptr ? m_config->edgePad : 0;
    return {
        .x = usable.x + edgePad,
        .y = usable.y + edgePad,
        .width = std::max(1, usable.width - 2 * edgePad),
        .height = std::max(1, usable.height - 2 * edgePad),
    };
  }

  int Layout::fractionalWidth(int viewportPrimary, double fraction) const {
    const int gap = m_config != nullptr ? m_config->totalGap : 0;
    return std::max(1, static_cast<int>(std::lround(fraction * (viewportPrimary + gap) - gap)));
  }

  bool tabBarShown(const ResolvedLayoutConfig& config, size_t tabs, std::optional<bool> bar) {
    if (bar) {
      return *bar;
    }
    return config.tabs.barVisible && (!config.tabs.hideWhenSingle || tabs > 1);
  }

  int Layout::tabBarReserve(size_t tabs, std::optional<bool> bar) const {
    if (m_config == nullptr || !tabBarShown(*m_config, tabs, bar)) {
      return 0;
    }
    return m_config->tabs.barHeight + m_config->tabs.barGap;
  }

  wlr_box Layout::tabbedBox(const wlr_box& box, size_t tabs, std::optional<bool> bar) const {
    const TabBarPosition position = m_config != nullptr ? m_config->tabs.barPosition : TabBarPosition::Top;
    const bool across = tabBarAcross(position);
    // A unit too small for its bar keeps one pixel for its tabs.
    const int reserve = std::min(tabBarReserve(tabs, bar), std::max(0, (across ? box.height : box.width) - 1));
    wlr_box shared = box;
    (across ? shared.height : shared.width) -= reserve;
    if (position == TabBarPosition::Top) {
      shared.y += reserve;
    } else if (position == TabBarPosition::Left) {
      shared.x += reserve;
    }
    return shared;
  }

  bool Layout::opensTabbed() const {
    return m_config != nullptr && m_config->tabs.defaultDisplay == ColumnDisplay::Tabbed;
  }

  TabState Layout::initialTabState() const {
    TabState tabs;
    tabs.setTabbed(opensTabbed(), 0);
    return tabs;
  }

  namespace {
    std::optional<size_t> rowIn(const Column& column, const View* view) {
      const auto it = std::ranges::find(column.views, view);
      if (view == nullptr || it == column.views.end()) {
        return std::nullopt;
      }
      return static_cast<size_t>(it - column.views.begin());
    }
  } // namespace

  const TabGroup* tabGroupOf(const Column& column, const View* view) {
    const std::optional<size_t> row = rowIn(column, view);
    return row ? column.tabs.groupAt(*row) : nullptr;
  }

  View* unitEntry(const Column& column, size_t row) {
    if (row >= column.views.size()) {
      return nullptr;
    }
    const size_t shown = column.tabs.unitShown(row);
    return shown < column.views.size() ? column.views[shown] : column.views[row];
  }

  View* columnEntry(const Column& column) { return unitEntry(column, 0); }

  bool hiddenTab(const Column& column, const View* view) {
    const std::optional<size_t> row = rowIn(column, view);
    return row && column.tabs.hidden(*row);
  }

  std::vector<View*> Layout::focusPeers(const View* from, const View* target) const {
    if (target == nullptr) {
      return {};
    }
    const int column = columnOf(target);
    if (column < 0 || column == columnOf(from)) {
      return {};
    }
    const Column& peers = columns()[static_cast<size_t>(column)];
    // Entering a column lands on a window it shows, never on a hidden tab, however recently it was focused.
    std::vector<View*> shown;
    shown.reserve(peers.views.size());
    for (size_t row = 0; row < peers.views.size(); ++row) {
      if (!peers.tabs.hidden(row)) {
        shown.push_back(peers.views[row]);
      }
    }
    return shown;
  }

  View* directionalNeighbor(std::span<const LayoutTarget> targets, const View* view, bool horizontal, int direction) {
    if (view == nullptr || direction == 0) {
      return nullptr;
    }

    const auto sourceIt =
        std::ranges::find_if(targets, [view](const LayoutTarget& target) { return target.view == view; });
    if (sourceIt == targets.end()) {
      return nullptr;
    }

    const LayoutTarget& source = *sourceIt;
    View* best = nullptr;
    int bestGap = std::numeric_limits<int>::max();
    int bestOverlap = -1;
    int bestCenterDistance = std::numeric_limits<int>::max();

    for (const LayoutTarget& candidate : targets) {
      if (candidate.view == nullptr || candidate.view == view) {
        continue;
      }

      int gap = 0;
      int overlap = 0;
      int centerDistance = 0;
      if (horizontal) {
        gap = direction < 0 ? source.x - (candidate.x + candidate.width) : candidate.x - (source.x + source.width);
        overlap = std::min(source.y + source.height, candidate.y + candidate.height) - std::max(source.y, candidate.y);
        centerDistance = std::abs((2 * source.y + source.height) - (2 * candidate.y + candidate.height));
      } else {
        gap = direction < 0 ? source.y - (candidate.y + candidate.height) : candidate.y - (source.y + source.height);
        overlap = std::min(source.x + source.width, candidate.x + candidate.width) - std::max(source.x, candidate.x);
        centerDistance = std::abs((2 * source.x + source.width) - (2 * candidate.x + candidate.width));
      }

      if (gap < 0 || overlap <= 0) {
        continue;
      }
      if (gap < bestGap
          || (gap == bestGap && overlap > bestOverlap)
          || (gap == bestGap && overlap == bestOverlap && centerDistance < bestCenterDistance)) {
        best = candidate.view;
        bestGap = gap;
        bestOverlap = overlap;
        bestCenterDistance = centerDistance;
      }
    }
    return best;
  }

  std::unique_ptr<Layout> createLayout(LayoutMode mode) {
    switch (mode) {
    case LayoutMode::Dwindle:
      return std::make_unique<DwindleLayout>();
    case LayoutMode::Master:
      return std::make_unique<MasterStackLayout>();
    case LayoutMode::Scrolling:
      return std::make_unique<ScrollingLayout>();
    case LayoutMode::Stacking:
      return std::make_unique<StackingLayout>();
    }
    std::unreachable();
  }

  uint32_t resizeEdgesForPoint(const wlr_box& box, double cx, double cy) {
    const double px = cx - box.x;
    const double py = cy - box.y;
    uint32_t edges = 0;
    if (px < box.width / 3.0) {
      edges |= WLR_EDGE_LEFT;
    } else if (px > 2.0 * box.width / 3.0) {
      edges |= WLR_EDGE_RIGHT;
    }
    if (py < box.height / 3.0) {
      edges |= WLR_EDGE_TOP;
    } else if (py > 2.0 * box.height / 3.0) {
      edges |= WLR_EDGE_BOTTOM;
    }
    return edges;
  }

} // namespace umbriel
