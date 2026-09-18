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

    struct MasterSnapshot final : LayoutSnapshot {
      struct Row {
        LayoutMemberId member = 0;
        double weight = 1.0;
      };

      [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Master; }
      [[nodiscard]] size_t memberCount() const override { return members; }

      std::vector<Row> master;
      std::vector<Row> stack;
      std::vector<Row> secondStack;
      size_t members = 0;
      double masterFraction = -1.0;
      double savedFraction = 0.0;
    };

    std::pair<int, int> columnWidths(int contentWidth, int gap, double masterFraction) {
      const int available = std::max(2, contentWidth - gap);
      const int masterWidth = std::clamp(static_cast<int>(std::lround(masterFraction * available)), 1, available - 1);
      return {masterWidth, available - masterWidth};
    }

    struct CenterWidths {
      int side = 0;
      int master = 0;
      int secondSide = 0;
    };

    // Center mode keeps the master box centered: both sides share what the master fraction leaves, so an odd
    // remainder goes to the second side.
    CenterWidths centerWidths(int contentWidth, int gap, double masterFraction) {
      const int available = std::max(3, contentWidth - 2 * gap);
      const int master = std::clamp(static_cast<int>(std::lround(masterFraction * available)), 1, available - 2);
      const int side = (available - master) / 2;
      return {.side = side, .master = master, .secondSide = available - master - side};
    }

    // Height the next stack row gets once it joins `stack`, matching what arrange will assign.
    int stackRowHeight(const MasterStackLayout::Area& stack, int contentHeight, int gap) {
      const int count = static_cast<int>(stack.views.size());
      const double weightSum = std::accumulate(stack.weights.begin(), stack.weights.end(), 0.0);
      const int available = contentHeight - count * gap;
      return std::max(1, static_cast<int>(std::lround(available / (weightSum + 1.0))));
    }

    class MasterResizeGrab final : public ResizeGrab {
    public:
      MasterResizeGrab(
          Layout* layout, double* masterFraction, double fraction0, double horizontalSpan, double horizontalSign,
          MasterStackLayout::Area* area, int upperRow, double weightSum, double upperHeight, double lowerHeight
      )
          : m_layout(layout), m_masterFraction(masterFraction), m_fraction0(fraction0),
            m_horizontalSpan(horizontalSpan), m_horizontalSign(horizontalSign), m_area(area), m_upperRow(upperRow),
            m_weightSum(weightSum), m_upperHeight(upperHeight), m_lowerHeight(lowerHeight) {}

      void applyDelta(double dx, double dy, const wlr_box& /*usable*/) override {
        if (m_masterFraction != nullptr && m_horizontalSpan > 0.0) {
          *m_masterFraction = std::clamp(m_fraction0 + m_horizontalSign * dx / m_horizontalSpan, 0.1, 0.9);
        }
        const double pairHeight = m_upperHeight + m_lowerHeight;
        if (m_area != nullptr
            && m_upperRow >= 0
            && static_cast<size_t>(m_upperRow + 1) < m_area->weights.size()
            && pairHeight > 0.0) {
          const double ratio = std::clamp((m_upperHeight + dy) / pairHeight, 0.05, 0.95);
          m_area->weights[static_cast<size_t>(m_upperRow)] = m_weightSum * ratio;
          m_area->weights[static_cast<size_t>(m_upperRow + 1)] = m_weightSum * (1.0 - ratio);
        }
      }

      [[nodiscard]] const Layout* ownerLayout() const override { return m_layout; }

    private:
      Layout* m_layout;
      double* m_masterFraction;
      double m_fraction0;
      double m_horizontalSpan;
      double m_horizontalSign;
      MasterStackLayout::Area* m_area;
      int m_upperRow;
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

  bool MasterStackLayout::masterIsCenter() const {
    return m_config != nullptr && m_config->master.position == MasterPosition::Center;
  }

  std::array<MasterStackLayout::Area*, 3> MasterStackLayout::orderedAreas() {
    const std::array<const Area*, 3> ordered = std::as_const(*this).orderedAreas();
    return {const_cast<Area*>(ordered[0]), const_cast<Area*>(ordered[1]), const_cast<Area*>(ordered[2])};
  }

  std::array<const MasterStackLayout::Area*, 3> MasterStackLayout::orderedAreas() const {
    if (masterIsCenter()) {
      return {&m_stack, &m_master, &m_secondStack};
    }
    if (masterIsLeft()) {
      return {&m_master, &m_stack, nullptr};
    }
    return {&m_stack, &m_master, nullptr};
  }

  MasterStackLayout::Area& MasterStackLayout::insertionStack() {
    return const_cast<Area&>(std::as_const(*this).insertionStack());
  }

  const MasterStackLayout::Area& MasterStackLayout::insertionStack() const {
    return masterIsCenter() && m_secondStack.views.size() < m_stack.views.size() ? m_secondStack : m_stack;
  }

  MasterStackLayout::Area* MasterStackLayout::promotionStack() {
    if (masterIsCenter() && m_secondStack.views.size() > m_stack.views.size()) {
      return &m_secondStack;
    }
    return m_stack.views.empty() ? nullptr : &m_stack;
  }

  void MasterStackLayout::foldSecondStack() {
    if (masterIsCenter() || m_secondStack.views.empty()) {
      return;
    }
    m_stack.views.insert(m_stack.views.end(), m_secondStack.views.begin(), m_secondStack.views.end());
    m_stack.weights.insert(m_stack.weights.end(), m_secondStack.weights.begin(), m_secondStack.weights.end());
    m_secondStack.views.clear();
    m_secondStack.weights.clear();
  }

  bool MasterStackLayout::widthAdjustable() const {
    return masterIsCenter() ? !m_master.views.empty() : !m_master.views.empty() && !m_stack.views.empty();
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
    if (std::ranges::find(m_secondStack.views, view) != m_secondStack.views.end()) {
      return &m_secondStack;
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
    int visibleIndex = 0;
    for (const Area* area : orderedAreas()) {
      if (area == nullptr || area->views.empty()) {
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
    saveArea(m_secondStack, snapshot->secondStack);
    snapshot->members = capture.members.size();
    snapshot->masterFraction = m_masterFrac;
    snapshot->savedFraction = m_savedFrac;
    return capture;
  }

  bool MasterStackLayout::restoreState(const LayoutSnapshot& base, std::span<const LayoutMember> members) {
    const auto* snapshot = dynamic_cast<const MasterSnapshot*>(&base);
    if (snapshot == nullptr || !m_master.views.empty() || !m_stack.views.empty() || !m_secondStack.views.empty()) {
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
    restoreArea(snapshot->secondStack, m_secondStack);
    if (m_master.views.empty()) {
      if (Area* stack = promotionStack(); stack != nullptr) {
        m_master.views.push_back(stack->views.front());
        m_master.weights.push_back(stack->weights.front());
        stack->views.erase(stack->views.begin());
        stack->weights.erase(stack->weights.begin());
      }
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
    erase(m_secondStack);
    std::erase_if(m_targets, [view](const LayoutTarget& target) { return target.view == view; });
  }

  void MasterStackLayout::rebuildColumns() {
    foldSecondStack();
    m_geometryStale = true;
    m_columns.clear();
    const double sideFrac = masterIsCenter() ? (1.0 - masterFrac()) / 2.0 : 1.0 - masterFrac();
    for (const Area* area : orderedAreas()) {
      if (area == nullptr || area->views.empty()) {
        continue;
      }
      Column column;
      column.views = area->views;
      column.heightWeights = area->weights;
      column.widthFrac = area == &m_master ? masterFrac() : sideFrac;
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
    } else if (m_config != nullptr && m_config->master.newBecomesMaster) {
      // The master count does not change, so the last master row drops to the stack top with its weight.
      Area& stack = insertionStack();
      stack.views.insert(stack.views.begin(), m_master.views.back());
      stack.weights.insert(stack.weights.begin(), m_master.weights.back());
      m_master.views.pop_back();
      m_master.weights.pop_back();
      m_master.views.insert(m_master.views.begin(), view);
      m_master.weights.insert(m_master.weights.begin(), 1.0);
    } else {
      const bool newOnTop = m_config == nullptr || m_config->master.newOnTop;
      Area& stack = insertionStack();
      stack.views.insert(newOnTop ? stack.views.begin() : stack.views.end(), view);
      stack.weights.insert(newOnTop ? stack.weights.begin() : stack.weights.end(), 1.0);
    }
    rebuildColumns();
  }

  void MasterStackLayout::insertViewIntoColumn(View* view, int columnIndex, int rowIndex) {
    if (view == nullptr) {
      return;
    }
    eraseFromAreas(view);

    Area* destination = nullptr;
    if (m_master.views.empty() && m_stack.views.empty() && m_secondStack.views.empty()) {
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

  bool MasterStackLayout::consume(View* view, int direction) {
    if (direction != -1 && direction != 1) {
      return false;
    }
    const std::array<Area*, 3> ordered = orderedAreas();
    int index = -1;
    for (int position = 0; position < static_cast<int>(ordered.size()); ++position) {
      if (ordered[position] != nullptr && rowInArea(*ordered[position], view) >= 0) {
        index = position;
        break;
      }
    }
    const int target = index + direction;
    if (index < 0 || target < 0 || target >= static_cast<int>(ordered.size()) || ordered[target] == nullptr) {
      return false;
    }
    Area* source = ordered[index];
    Area* destination = ordered[target];
    const int row = rowInArea(*source, view);
    const double weight = source->weights[static_cast<size_t>(row)];
    source->views.erase(source->views.begin() + row);
    source->weights.erase(source->weights.begin() + row);
    destination->views.push_back(view);
    destination->weights.push_back(weight);
    rebuildColumns();
    return true;
  }

  bool MasterStackLayout::expel(View* view, int direction) { return consume(view, direction); }

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

  bool MasterStackLayout::swapViews(View* a, View* b) {
    if (a == b) {
      return false;
    }
    Area* firstArea = areaOf(a);
    Area* secondArea = areaOf(b);
    if (firstArea == nullptr || secondArea == nullptr) {
      return false;
    }
    const int first = rowInArea(*firstArea, a);
    const int second = rowInArea(*secondArea, b);
    if (first < 0 || second < 0) {
      return false;
    }
    std::swap(firstArea->views[static_cast<size_t>(first)], secondArea->views[static_cast<size_t>(second)]);
    for (LayoutTarget& target : m_targets) {
      if (target.view == a) {
        target.view = b;
      } else if (target.view == b) {
        target.view = a;
      }
    }
    rebuildColumns();
    return true;
  }

  bool MasterStackLayout::promoteFromStack() {
    Area* stack = promotionStack();
    if (stack == nullptr) {
      return false;
    }
    m_master.views.push_back(stack->views.front());
    m_master.weights.push_back(stack->weights.front());
    stack->views.erase(stack->views.begin());
    stack->weights.erase(stack->weights.begin());
    rebuildColumns();
    return true;
  }

  bool MasterStackLayout::demoteToStack() {
    if (m_master.views.size() < 2) {
      return false;
    }
    Area& stack = insertionStack();
    stack.views.insert(stack.views.begin(), m_master.views.back());
    stack.weights.insert(stack.weights.begin(), m_master.weights.back());
    m_master.views.pop_back();
    m_master.weights.pop_back();
    rebuildColumns();
    return true;
  }

  void MasterStackLayout::removeView(View* view) {
    const bool wasMaster = rowInArea(m_master, view) >= 0;
    if (!wasMaster && areaOf(view) == nullptr) {
      return;
    }
    eraseFromAreas(view);
    if (wasMaster && m_master.views.empty()) {
      if (Area* stack = promotionStack(); stack != nullptr) {
        m_master.views.push_back(stack->views.front());
        m_master.weights.push_back(stack->weights.front());
        stack->views.erase(stack->views.begin());
        stack->weights.erase(stack->weights.begin());
      }
    }
    rebuildColumns();
  }

  void MasterStackLayout::moveColumn(int from, int to) {
    if (from == to) {
      return;
    }
    Area* source = visualArea(from);
    Area* destination = visualArea(to);
    if (source == nullptr || destination == nullptr || source == destination) {
      return;
    }
    std::swap(*source, *destination);
    rebuildColumns();
  }

  void MasterStackLayout::arrange(const wlr_box& usable) {
    foldSecondStack();
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

    if (masterIsCenter()) {
      if (!m_master.views.empty()) {
        const CenterWidths widths = centerWidths(content.width, gap, masterFrac());
        const wlr_box left{
            .x = content.x,
            .y = content.y,
            .width = widths.side,
            .height = content.height,
        };
        const wlr_box middle{
            .x = left.x + widths.side + gap,
            .y = content.y,
            .width = widths.master,
            .height = content.height,
        };
        const wlr_box right{
            .x = middle.x + widths.master + gap,
            .y = content.y,
            .width = widths.secondSide,
            .height = content.height,
        };
        arrangeArea(m_stack, left);
        arrangeArea(m_master, middle);
        arrangeArea(m_secondStack, right);
      } else if (m_stack.views.empty() || m_secondStack.views.empty()) {
        arrangeArea(m_stack, content);
        arrangeArea(m_secondStack, content);
      } else {
        const int leftWidth = (content.width - gap) / 2;
        const wlr_box left{.x = content.x, .y = content.y, .width = leftWidth, .height = content.height};
        const wlr_box right{
            .x = left.x + leftWidth + gap,
            .y = content.y,
            .width = content.width - gap - leftWidth,
            .height = content.height,
        };
        arrangeArea(m_stack, left);
        arrangeArea(m_secondStack, right);
      }
    } else if (m_master.views.empty()) {
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
    m_geometryStale = false;
  }

  wlr_box MasterStackLayout::targetBox(const View* view) const {
    const auto it = std::ranges::find_if(m_targets, [view](const LayoutTarget& target) { return target.view == view; });
    if (it == m_targets.end()) {
      return {};
    }
    return {.x = it->x, .y = it->y, .width = it->width, .height = it->height};
  }

  Layout::InitialSize MasterStackLayout::initialSize(
      const wlr_box& usable, bool /*wantMaximize*/, std::optional<double> /*ruleExtent*/,
      std::optional<int> /*ruleExtentPx*/, const View* /*splitAnchor*/
  ) const {
    const wlr_box content = contentArea(usable);
    const int gap = m_config != nullptr ? m_config->totalGap : 0;
    const bool becomesMaster = m_master.views.empty() || (m_config != nullptr && m_config->master.newBecomesMaster);

    if (masterIsCenter()) {
      const CenterWidths widths = centerWidths(content.width, gap, masterFrac());
      if (becomesMaster) {
        return {.width = widths.master, .height = content.height};
      }
      const Area& stack = insertionStack();
      return {
          .width = &stack == &m_secondStack ? widths.secondSide : widths.side,
          .height = stackRowHeight(stack, content.height, gap),
      };
    }

    if (m_master.views.empty() && m_stack.views.empty()) {
      return {.width = content.width, .height = content.height};
    }
    const auto [masterWidth, stackWidth] = columnWidths(content.width, gap, masterFrac());
    if (becomesMaster) {
      return {.width = masterWidth, .height = content.height};
    }
    return {.width = stackWidth, .height = stackRowHeight(m_stack, content.height, gap)};
  }

  std::optional<View*> MasterStackLayout::focusHorizontalLeaf(const View* view, int direction) const {
    // A structural change invalidates the boxes until the next arrange. Answering from them would send focus by the
    // old geometry, so the caller falls back to column and row order instead.
    if (m_geometryStale) {
      return std::nullopt;
    }
    return directionalNeighbor(m_targets, view, true, direction);
  }

  std::optional<View*> MasterStackLayout::focusVerticalLeaf(const View* view, int direction) const {
    if (m_geometryStale) {
      return std::nullopt;
    }
    return directionalNeighbor(m_targets, view, false, direction);
  }

  bool MasterStackLayout::cycleWidth(int columnIndex, int direction) {
    if (!widthAdjustable() || visualArea(columnIndex) == nullptr) {
      return false;
    }
    const double next = nextFractionPreset(m_config->extentPresets, widthFraction(columnIndex), direction);
    return setWidthFraction(columnIndex, next);
  }

  bool MasterStackLayout::toggleFullWidth(int columnIndex) {
    if (!widthAdjustable() || visualArea(columnIndex) == nullptr) {
      return false;
    }
    const Area* area = visualArea(columnIndex);
    const double current = widthFraction(columnIndex);
    if (current >= kFullWidth - kFractionEpsilon) {
      double restore = m_savedFrac;
      if (restore <= 0.0) {
        const double defaultFraction = m_config->master.defaultWidthFraction;
        if (area == &m_master) {
          restore = defaultFraction;
        } else {
          restore = masterIsCenter() ? (1.0 - defaultFraction) / 2.0 : 1.0 - defaultFraction;
        }
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
    return widthAdjustable()
        && visualArea(columnIndex) != nullptr
        && widthFraction(columnIndex) >= kFullWidth - kFractionEpsilon;
  }

  bool MasterStackLayout::setWidthFraction(int columnIndex, double fraction) {
    if (!widthAdjustable()) {
      return false;
    }
    const Area* area = visualArea(columnIndex);
    if (area == nullptr) {
      return false;
    }
    const double used = std::clamp(fraction, 0.1, 0.9);
    if (area == &m_master) {
      m_masterFrac = used;
    } else {
      // A side column owns half of what the master leaves, so its fraction sets the master's complement.
      m_masterFrac = masterIsCenter() ? 1.0 - 2.0 * used : 1.0 - used;
    }
    m_masterFrac = std::clamp(m_masterFrac, 0.1, 0.9);
    m_savedFrac = 0.0;
    rebuildColumns();
    return true;
  }

  void MasterStackLayout::clearFullWidthState(int columnIndex) {
    if (widthAdjustable() && visualArea(columnIndex) != nullptr) {
      m_savedFrac = 0.0;
    }
  }

  double MasterStackLayout::widthFraction(int columnIndex) const {
    const Area* area = visualArea(columnIndex);
    if (area == nullptr) {
      return 1.0;
    }
    if (masterIsCenter()) {
      if (m_master.views.empty()) {
        return m_stack.views.empty() || m_secondStack.views.empty() ? 1.0 : 0.5;
      }
      return area == &m_master ? masterFrac() : (1.0 - masterFrac()) / 2.0;
    }
    if (!widthAdjustable()) {
      return 1.0;
    }
    return area == &m_master ? masterFrac() : 1.0 - masterFrac();
  }

  double MasterStackLayout::heightFraction(const View* view) const {
    const Area* area = areaOf(view);
    if (area == nullptr || area->views.size() <= 1) {
      return 1.0;
    }
    const int row = rowInArea(*area, view);
    const double total = std::accumulate(area->weights.begin(), area->weights.end(), 0.0);
    return area->weights[static_cast<size_t>(row)] / total;
  }

  bool MasterStackLayout::setHeightFraction(View* view, double fraction) {
    Area* area = areaOf(view);
    if (area == nullptr || area->views.size() <= 1) {
      return false;
    }
    const int row = rowInArea(*area, view);
    const auto index = static_cast<size_t>(row);
    const double total = std::accumulate(area->weights.begin(), area->weights.end(), 0.0);
    const double others = total - area->weights[index];
    const double target = std::clamp(fraction, 0.1, 0.95);
    area->weights[index] = target * others / (1.0 - target);
    rebuildColumns();
    return true;
  }

  uint32_t MasterStackLayout::resizableEdges(const View* view) const {
    const Area* area = areaOf(view);
    if (area == nullptr) {
      return 0;
    }
    uint32_t edges = 0;
    if (masterIsCenter()) {
      if (!m_master.views.empty()) {
        // The master box is centered, so both of its margins move; a side column only borders the master.
        if (area == &m_master) {
          edges |= WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
        } else {
          edges |= area == &m_stack ? WLR_EDGE_RIGHT : WLR_EDGE_LEFT;
        }
      }
    } else if (!m_master.views.empty() && !m_stack.views.empty()) {
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
      if (masterIsCenter()) {
        // Both margins move together, so the master grows by twice the edge travel.
        horizontalSpan = (contentArea(usable).width - 2 * m_config->totalGap) / 2.0;
        if (area == &m_master) {
          horizontalSign = (allowed & WLR_EDGE_RIGHT) != 0 ? 1.0 : -1.0;
        } else {
          horizontalSign = area == &m_stack ? -1.0 : 1.0;
        }
      } else {
        horizontalSpan = static_cast<double>(contentArea(usable).width - m_config->totalGap);
        horizontalSign = masterIsLeft() ? 1.0 : -1.0;
      }
    }

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
      weightSum = area->weights[static_cast<size_t>(upperRow)] + area->weights[static_cast<size_t>(upperRow + 1)];
      upperHeight = targetBox(area->views[static_cast<size_t>(upperRow)]).height;
      lowerHeight = targetBox(area->views[static_cast<size_t>(upperRow + 1)]).height;
    }

    if (horizontalFraction == nullptr && upperRow < 0) {
      return nullptr;
    }
    m_savedFrac = 0.0;
    return std::make_unique<MasterResizeGrab>(
        this, horizontalFraction, masterFrac(), horizontalSpan, horizontalSign, area, upperRow, weightSum, upperHeight,
        lowerHeight
    );
  }

} // namespace umbriel
