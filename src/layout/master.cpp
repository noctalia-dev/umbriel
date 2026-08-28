#include "layout/master.h"

#include "config/config.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <ranges>
#include <utility>

extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}

namespace umbriel {

  namespace {

    constexpr double kFullWidth = 0.9;
    constexpr double kFractionEpsilon = 0.0001;

    struct MasterSnapshot final : LayoutSnapshot {
      struct Row {
        LayoutMemberId member = 0;
        double weight = 1.0;
      };

      [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Master; }
      [[nodiscard]] size_t memberCount() const override { return members; }

      std::vector<Row> master;
      std::vector<Row> stack;
      size_t members = 0;
      double masterFraction = -1.0;
      double savedFraction = 0.0;
    };

    std::pair<int, int> columnWidths(int contentWidth, int gap, double masterFraction) {
      const int available = std::max(2, contentWidth - gap);
      const int masterWidth = std::clamp(static_cast<int>(std::lround(masterFraction * available)), 1, available - 1);
      return {masterWidth, available - masterWidth};
    }

    class MasterResizeGrab final : public ResizeGrab {
    public:
      MasterResizeGrab(
          Layout* layout, double* masterFraction, double fraction0, double horizontalSpan, double horizontalSign,
          double* upperWeight, double* lowerWeight, double weightSum, double upperHeight, double lowerHeight
      )
          : m_layout(layout), m_masterFraction(masterFraction), m_fraction0(fraction0),
            m_horizontalSpan(horizontalSpan), m_horizontalSign(horizontalSign), m_upperWeight(upperWeight),
            m_lowerWeight(lowerWeight), m_weightSum(weightSum), m_upperHeight(upperHeight), m_lowerHeight(lowerHeight) {
      }

      void applyDelta(double dx, double dy, const wlr_box& /*usable*/) override {
        if (m_masterFraction != nullptr && m_horizontalSpan > 0.0) {
          *m_masterFraction = std::clamp(m_fraction0 + m_horizontalSign * dx / m_horizontalSpan, 0.1, 0.9);
        }
        const double pairHeight = m_upperHeight + m_lowerHeight;
        if (m_upperWeight != nullptr && m_lowerWeight != nullptr && pairHeight > 0.0) {
          const double ratio = std::clamp((m_upperHeight + dy) / pairHeight, 0.05, 0.95);
          *m_upperWeight = m_weightSum * ratio;
          *m_lowerWeight = m_weightSum * (1.0 - ratio);
        }
      }

      [[nodiscard]] const Layout* ownerLayout() const override { return m_layout; }

    private:
      Layout* m_layout;
      double* m_masterFraction;
      double m_fraction0;
      double m_horizontalSpan;
      double m_horizontalSign;
      double* m_upperWeight;
      double* m_lowerWeight;
      double m_weightSum;
      double m_upperHeight;
      double m_lowerHeight;
    };

  } // namespace

  double MasterStackLayout::masterFrac() const {
    const double fraction =
        m_masterFrac >= 0.0 ? m_masterFrac : (m_config != nullptr ? m_config->master.defaultWidthFraction : 0.55);
    return std::clamp(fraction, 0.1, 0.9);
  }

  bool MasterStackLayout::masterIsLeft() const {
    return m_config == nullptr || m_config->master.position == MasterPosition::Left;
  }

  MasterStackLayout::Area* MasterStackLayout::areaOf(const View* view) {
    return const_cast<Area*>(std::as_const(*this).areaOf(view));
  }

  const MasterStackLayout::Area* MasterStackLayout::areaOf(const View* view) const {
    if (std::ranges::find(m_master.views, view) != m_master.views.end()) {
      return &m_master;
    }
    if (std::ranges::find(m_stack.views, view) != m_stack.views.end()) {
      return &m_stack;
    }
    return nullptr;
  }

  MasterStackLayout::Area* MasterStackLayout::visualArea(int columnIndex) {
    return const_cast<Area*>(std::as_const(*this).visualArea(columnIndex));
  }

  const MasterStackLayout::Area* MasterStackLayout::visualArea(int columnIndex) const {
    if (columnIndex < 0) {
      return nullptr;
    }
    const Area* ordered[2] = {
        masterIsLeft() ? &m_master : &m_stack,
        masterIsLeft() ? &m_stack : &m_master,
    };
    int visibleIndex = 0;
    for (const Area* area : ordered) {
      if (area->views.empty()) {
        continue;
      }
      if (visibleIndex == columnIndex) {
        return area;
      }
      ++visibleIndex;
    }
    return nullptr;
  }

  int MasterStackLayout::rowInArea(const Area& area, const View* view) const {
    const auto it = std::ranges::find(area.views, view);
    return it == area.views.end() ? -1 : static_cast<int>(it - area.views.begin());
  }

  int MasterStackLayout::columnOf(const View* view) const {
    for (size_t column = 0; column < m_columns.size(); ++column) {
      if (std::ranges::find(m_columns[column].views, view) != m_columns[column].views.end()) {
        return static_cast<int>(column);
      }
    }
    return -1;
  }

  int MasterStackLayout::rowOf(const View* view) const {
    const Area* area = areaOf(view);
    return area != nullptr ? rowInArea(*area, view) : -1;
  }

  LayoutCapture MasterStackLayout::captureState() const {
    auto snapshot = std::make_shared<MasterSnapshot>();
    LayoutCapture capture{.snapshot = snapshot, .members = {}};
    const auto saveArea = [&capture](const Area& area, std::vector<MasterSnapshot::Row>& rows) {
      rows.reserve(area.views.size());
      for (size_t row = 0; row < area.views.size(); ++row) {
        const auto id = static_cast<LayoutMemberId>(capture.members.size());
        capture.members.push_back({.id = id, .view = area.views[row]});
        rows.push_back({.member = id, .weight = row < area.weights.size() ? area.weights[row] : 1.0});
      }
    };
    saveArea(m_master, snapshot->master);
    saveArea(m_stack, snapshot->stack);
    snapshot->members = capture.members.size();
    snapshot->masterFraction = m_masterFrac;
    snapshot->savedFraction = m_savedFrac;
    return capture;
  }

  bool MasterStackLayout::restoreState(const LayoutSnapshot& base, std::span<const LayoutMember> members) {
    const auto* snapshot = dynamic_cast<const MasterSnapshot*>(&base);
    if (snapshot == nullptr || !m_master.views.empty() || !m_stack.views.empty()) {
      return false;
    }
    const std::optional<std::vector<View*>> resolved = resolveLayoutMembers(snapshot->memberCount(), members);
    if (!resolved) {
      return false;
    }

    const auto restoreArea = [&resolved](const std::vector<MasterSnapshot::Row>& rows, Area& area) {
      for (const MasterSnapshot::Row& row : rows) {
        View* view = (*resolved)[static_cast<size_t>(row.member)];
        if (view != nullptr) {
          area.views.push_back(view);
          area.weights.push_back(row.weight);
        }
      }
    };
    restoreArea(snapshot->master, m_master);
    restoreArea(snapshot->stack, m_stack);
    if (m_master.views.empty() && !m_stack.views.empty()) {
      m_master.views.push_back(m_stack.views.front());
      m_master.weights.push_back(m_stack.weights.front());
      m_stack.views.erase(m_stack.views.begin());
      m_stack.weights.erase(m_stack.weights.begin());
    }
    m_masterFrac = snapshot->masterFraction;
    m_savedFrac = snapshot->savedFraction;
    m_targets.clear();
    rebuildColumns();
    return true;
  }

  void MasterStackLayout::eraseFromAreas(View* view) {
    const auto erase = [view](Area& area) {
      const auto it = std::ranges::find(area.views, view);
      if (it == area.views.end()) {
        return;
      }
      const auto index = static_cast<size_t>(it - area.views.begin());
      area.views.erase(it);
      area.weights.erase(area.weights.begin() + static_cast<std::ptrdiff_t>(index));
    };
    erase(m_master);
    erase(m_stack);
    std::erase_if(m_targets, [view](const LayoutTarget& target) { return target.view == view; });
  }

  void MasterStackLayout::rebuildColumns() {
    m_columns.clear();
    const Area* ordered[2] = {
        masterIsLeft() ? &m_master : &m_stack,
        masterIsLeft() ? &m_stack : &m_master,
    };
    for (const Area* area : ordered) {
      if (area->views.empty()) {
        continue;
      }
      Column column;
      column.views = area->views;
      column.heightWeights = area->weights;
      column.widthFrac = area == &m_master ? masterFrac() : 1.0 - masterFrac();
      m_columns.push_back(std::move(column));
    }
  }

  void MasterStackLayout::insertView(View* view, int /*columnIndex*/) {
    if (view == nullptr) {
      return;
    }
    eraseFromAreas(view);
    if (m_master.views.empty()) {
      m_master.views.push_back(view);
      m_master.weights.push_back(1.0);
    } else {
      m_stack.views.insert(m_stack.views.begin(), view);
      m_stack.weights.insert(m_stack.weights.begin(), 1.0);
    }
    rebuildColumns();
  }

  void MasterStackLayout::insertViewIntoColumn(View* view, int columnIndex, int rowIndex) {
    if (view == nullptr) {
      return;
    }
    eraseFromAreas(view);

    Area* destination = nullptr;
    if (m_master.views.empty() && m_stack.views.empty()) {
      destination = &m_master;
    } else {
      destination = visualArea(columnIndex);
    }
    if (destination == nullptr) {
      rebuildColumns();
      return;
    }

    const int row = std::clamp(rowIndex, 0, static_cast<int>(destination->views.size()));
    destination->views.insert(destination->views.begin() + row, view);
    destination->weights.insert(destination->weights.begin() + row, 1.0);
    rebuildColumns();
  }

  bool MasterStackLayout::consumeLeft(View* view) {
    Area* source = masterIsLeft() ? &m_stack : &m_master;
    Area* destination = masterIsLeft() ? &m_master : &m_stack;
    const int row = rowInArea(*source, view);
    if (row < 0) {
      return false;
    }
    const double weight = source->weights[static_cast<size_t>(row)];
    source->views.erase(source->views.begin() + row);
    source->weights.erase(source->weights.begin() + row);
    destination->views.push_back(view);
    destination->weights.push_back(weight);
    rebuildColumns();
    return true;
  }

  bool MasterStackLayout::expelRight(View* view) {
    Area* source = masterIsLeft() ? &m_master : &m_stack;
    Area* destination = masterIsLeft() ? &m_stack : &m_master;
    const int row = rowInArea(*source, view);
    if (row < 0) {
      return false;
    }
    const double weight = source->weights[static_cast<size_t>(row)];
    source->views.erase(source->views.begin() + row);
    source->weights.erase(source->weights.begin() + row);
    destination->views.push_back(view);
    destination->weights.push_back(weight);
    rebuildColumns();
    return true;
  }

  bool MasterStackLayout::moveViewVertical(View* view, int direction) {
    View* neighbor = directionalNeighbor(m_targets, view, false, direction);
    if (neighbor == nullptr) {
      return false;
    }
    Area* area = areaOf(view);
    if (area == nullptr || area != areaOf(neighbor)) {
      return false;
    }
    const int first = rowInArea(*area, view);
    const int second = rowInArea(*area, neighbor);
    if (first < 0 || second < 0) {
      return false;
    }
    std::swap(area->views[static_cast<size_t>(first)], area->views[static_cast<size_t>(second)]);
    for (LayoutTarget& target : m_targets) {
      if (target.view == view) {
        target.view = neighbor;
      } else if (target.view == neighbor) {
        target.view = view;
      }
    }
    rebuildColumns();
    return true;
  }

  void MasterStackLayout::removeView(View* view) {
    const bool wasMaster = rowInArea(m_master, view) >= 0;
    if (!wasMaster && rowInArea(m_stack, view) < 0) {
      return;
    }
    eraseFromAreas(view);
    if (wasMaster && m_master.views.empty() && !m_stack.views.empty()) {
      m_master.views.push_back(m_stack.views.front());
      m_master.weights.push_back(m_stack.weights.front());
      m_stack.views.erase(m_stack.views.begin());
      m_stack.weights.erase(m_stack.weights.begin());
    }
    rebuildColumns();
  }

  void MasterStackLayout::moveColumn(int from, int to) {
    if (m_master.views.empty()
        || m_stack.views.empty()
        || from < 0
        || to < 0
        || from >= static_cast<int>(m_columns.size())
        || to >= static_cast<int>(m_columns.size())
        || from == to) {
      return;
    }
    std::swap(m_master, m_stack);
    rebuildColumns();
  }

  void MasterStackLayout::arrange(const wlr_box& usable) {
    m_targets.clear();
    const wlr_box content = contentArea(usable);
    const int gap = m_config != nullptr ? m_config->totalGap : 0;

    const auto arrangeArea = [&](const Area& area, const wlr_box& box) {
      if (area.views.empty()) {
        return;
      }
      const int rowCount = static_cast<int>(area.views.size());
      const int available = box.height - std::max(0, rowCount - 1) * gap;
      const double weightSum = std::accumulate(area.weights.begin(), area.weights.end(), 0.0);
      int y = box.y;
      int used = 0;
      for (int row = 0; row < rowCount; ++row) {
        int height = 1;
        if (row == rowCount - 1) {
          height = std::max(1, available - used);
        } else {
          height = std::max(
              1, static_cast<int>(std::lround(available * area.weights[static_cast<size_t>(row)] / weightSum))
          );
          used += height;
        }
        m_targets.push_back({
            .view = area.views[static_cast<size_t>(row)],
            .x = box.x,
            .y = y,
            .width = box.width,
            .height = height,
        });
        y += height + gap;
      }
    };

    if (m_master.views.empty()) {
      arrangeArea(m_stack, content);
    } else if (m_stack.views.empty()) {
      arrangeArea(m_master, content);
    } else {
      const auto [masterWidth, stackWidth] = columnWidths(content.width, gap, masterFrac());
      const wlr_box left{
          .x = content.x,
          .y = content.y,
          .width = masterIsLeft() ? masterWidth : stackWidth,
          .height = content.height,
      };
      const wlr_box right{
          .x = left.x + left.width + gap,
          .y = content.y,
          .width = masterIsLeft() ? stackWidth : masterWidth,
          .height = content.height,
      };
      arrangeArea(masterIsLeft() ? m_master : m_stack, left);
      arrangeArea(masterIsLeft() ? m_stack : m_master, right);
    }
    rebuildColumns();
  }

  wlr_box MasterStackLayout::targetBox(const View* view) const {
    const auto it = std::ranges::find_if(m_targets, [view](const LayoutTarget& target) { return target.view == view; });
    if (it == m_targets.end()) {
      return {};
    }
    return {.x = it->x, .y = it->y, .width = it->width, .height = it->height};
  }

  Layout::InitialSize MasterStackLayout::initialSize(
      const wlr_box& usable, std::optional<double> /*ruleWidthFraction*/, const View* /*splitAnchor*/
  ) const {
    const wlr_box content = contentArea(usable);
    if (m_master.views.empty() && m_stack.views.empty()) {
      return {.width = content.width, .height = content.height};
    }

    const int gap = m_config != nullptr ? m_config->totalGap : 0;
    const auto [masterWidth, stackWidth] = columnWidths(content.width, gap, masterFrac());
    if (m_master.views.empty()) {
      return {.width = masterWidth, .height = content.height};
    }

    const int count = static_cast<int>(m_stack.views.size());
    const double weightSum = std::accumulate(m_stack.weights.begin(), m_stack.weights.end(), 0.0);
    const int available = content.height - count * gap;
    const int height = std::max(1, static_cast<int>(std::lround(available / (weightSum + 1.0))));
    return {.width = stackWidth, .height = height};
  }

  std::optional<View*> MasterStackLayout::focusHorizontalLeaf(const View* view, int direction) const {
    return directionalNeighbor(m_targets, view, true, direction);
  }

  std::optional<View*> MasterStackLayout::focusVerticalLeaf(const View* view, int direction) const {
    return directionalNeighbor(m_targets, view, false, direction);
  }

  bool MasterStackLayout::cycleWidth(int columnIndex, int direction) {
    if (m_master.views.empty() || m_stack.views.empty() || visualArea(columnIndex) == nullptr) {
      return false;
    }
    const auto& presets = m_config->widthPresets;
    const double current = widthFraction(columnIndex);
    double next = current;
    if (direction < 0) {
      const auto it = std::ranges::find_if(presets | std::views::reverse, [current](double preset) {
        return preset < current - kFractionEpsilon;
      });
      next = it == presets.rend() ? presets.back() : *it;
    } else {
      const auto it =
          std::ranges::find_if(presets, [current](double preset) { return preset > current + kFractionEpsilon; });
      next = it == presets.end() ? presets.front() : *it;
    }
    return setWidthFraction(columnIndex, next);
  }

  bool MasterStackLayout::toggleFullWidth(int columnIndex) {
    if (m_master.views.empty() || m_stack.views.empty() || visualArea(columnIndex) == nullptr) {
      return false;
    }
    const Area* area = visualArea(columnIndex);
    const double current = widthFraction(columnIndex);
    if (current >= kFullWidth - kFractionEpsilon) {
      double restore = m_savedFrac;
      if (restore <= 0.0) {
        restore =
            area == &m_master ? m_config->master.defaultWidthFraction : 1.0 - m_config->master.defaultWidthFraction;
      }
      m_savedFrac = 0.0;
      setWidthFraction(columnIndex, restore);
      return false;
    }
    m_savedFrac = current;
    setWidthFraction(columnIndex, kFullWidth);
    m_savedFrac = current;
    return true;
  }

  bool MasterStackLayout::isFullWidth(int columnIndex) const {
    return !m_master.views.empty()
        && !m_stack.views.empty()
        && visualArea(columnIndex) != nullptr
        && widthFraction(columnIndex) >= kFullWidth - kFractionEpsilon;
  }

  bool MasterStackLayout::setWidthFraction(int columnIndex, double fraction) {
    if (m_master.views.empty() || m_stack.views.empty()) {
      return false;
    }
    const Area* area = visualArea(columnIndex);
    if (area == nullptr) {
      return false;
    }
    const double used = std::clamp(fraction, 0.1, 0.9);
    m_masterFrac = area == &m_master ? used : 1.0 - used;
    m_masterFrac = std::clamp(m_masterFrac, 0.1, 0.9);
    m_savedFrac = 0.0;
    rebuildColumns();
    return true;
  }

  void MasterStackLayout::clearFullWidthState(int columnIndex) {
    if (!m_master.views.empty() && !m_stack.views.empty() && visualArea(columnIndex) != nullptr) {
      m_savedFrac = 0.0;
    }
  }

  double MasterStackLayout::widthFraction(int columnIndex) const {
    if (m_master.views.empty() || m_stack.views.empty()) {
      return 1.0;
    }
    const Area* area = visualArea(columnIndex);
    if (area == nullptr) {
      return 1.0;
    }
    return area == &m_master ? masterFrac() : 1.0 - masterFrac();
  }

  uint32_t MasterStackLayout::resizableEdges(const View* view) const {
    const Area* area = areaOf(view);
    if (area == nullptr) {
      return 0;
    }
    uint32_t edges = 0;
    if (!m_master.views.empty() && !m_stack.views.empty()) {
      const bool areaIsLeft = (area == &m_master) == masterIsLeft();
      edges |= areaIsLeft ? WLR_EDGE_RIGHT : WLR_EDGE_LEFT;
    }
    const int row = rowInArea(*area, view);
    if (row > 0) {
      edges |= WLR_EDGE_TOP;
    }
    if (row >= 0 && row + 1 < static_cast<int>(area->views.size())) {
      edges |= WLR_EDGE_BOTTOM;
    }
    return edges;
  }

  uint32_t MasterStackLayout::resizeEdgesAt(const View* view, double cx, double cy) const {
    const wlr_box box = targetBox(view);
    if (box.width <= 0 || box.height <= 0) {
      return 0;
    }
    return sanitizeResizeEdges(view, resizeEdgesForPoint(box, cx, cy));
  }

  uint32_t MasterStackLayout::sanitizeResizeEdges(const View* view, uint32_t edges) const {
    return edges & resizableEdges(view);
  }

  std::unique_ptr<ResizeGrab> MasterStackLayout::beginResize(View* view, uint32_t edges, const wlr_box& usable) {
    Area* area = areaOf(view);
    if (area == nullptr) {
      return nullptr;
    }
    const uint32_t allowed = edges & resizableEdges(view);

    double* horizontalFraction = nullptr;
    double horizontalSpan = 0.0;
    double horizontalSign = 0.0;
    if ((allowed & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) != 0) {
      horizontalFraction = &m_masterFrac;
      horizontalSpan = static_cast<double>(contentArea(usable).width - m_config->totalGap);
      horizontalSign = masterIsLeft() ? 1.0 : -1.0;
    }

    double* upperWeight = nullptr;
    double* lowerWeight = nullptr;
    double weightSum = 0.0;
    double upperHeight = 0.0;
    double lowerHeight = 0.0;
    int upperRow = -1;
    const int row = rowInArea(*area, view);
    if ((allowed & WLR_EDGE_TOP) != 0 && row > 0) {
      upperRow = row - 1;
    } else if ((allowed & WLR_EDGE_BOTTOM) != 0 && row + 1 < static_cast<int>(area->views.size())) {
      upperRow = row;
    }
    if (upperRow >= 0) {
      upperWeight = &area->weights[static_cast<size_t>(upperRow)];
      lowerWeight = &area->weights[static_cast<size_t>(upperRow + 1)];
      weightSum = *upperWeight + *lowerWeight;
      upperHeight = targetBox(area->views[static_cast<size_t>(upperRow)]).height;
      lowerHeight = targetBox(area->views[static_cast<size_t>(upperRow + 1)]).height;
    }

    if (horizontalFraction == nullptr && upperWeight == nullptr) {
      return nullptr;
    }
    m_savedFrac = 0.0;
    return std::make_unique<MasterResizeGrab>(
        this, horizontalFraction, masterFrac(), horizontalSpan, horizontalSign, upperWeight, lowerWeight, weightSum,
        upperHeight, lowerHeight
    );
  }

} // namespace umbriel
