#include "layout/scrolling.h"

#include "config/config.h"
#include "view/floating.h"

#include <algorithm>
#include <cmath>
#include <ranges>

// wlr_box and WLR_EDGE_* only. Layout geometry must not pull src/wlr.h, which
// drags SceneFX and the renderer into a translation unit that does arithmetic.
extern "C" {
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
}

namespace umbriel {

  namespace {

    constexpr double kMinHeightWeight = 0.05;

    struct ScrollingSnapshot final : LayoutSnapshot {
      struct Row {
        LayoutMemberId member = 0;
        double heightWeight = 1.0;
      };

      struct SavedColumn {
        std::vector<Row> rows;
        double topGapWeight = 0.0;
        double bottomGapWeight = 0.0;
        double widthFraction = 0.5;
        double savedWidthFraction = 0.0;
        double viewportCenterFraction = 0.5;
      };

      [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Scrolling; }
      [[nodiscard]] size_t memberCount() const override { return members; }

      std::vector<SavedColumn> columns;
      size_t members = 0;
      double scroll = 0.0;
      bool centeredRest = false;
      int viewportPrimary = 0;
      bool exactViewportRestorable = false;
    };

    void ensureWeightCount(Column& column) {
      while (column.heightWeights.size() < column.views.size()) {
        column.heightWeights.push_back(1.0);
      }
      if (column.heightWeights.size() > column.views.size()) {
        column.heightWeights.resize(column.views.size());
      }
    }

    double columnTotalWeight(const Column& column) {
      double total = std::max(0.0, column.topGapWeight) + std::max(0.0, column.bottomGapWeight);
      for (double weight : column.heightWeights) {
        total += std::max(kMinHeightWeight, weight);
      }
      return std::max(kMinHeightWeight, total);
    }

    int columnMinPrimaryPx(const Column& column, const Layout& layout) {
      const bool vertical = layout.layoutConfig()->scrolling.direction == ScrollingDirection::Vertical;
      int minimum = 1;
      for (const View* view : column.views) {
        if (view == nullptr) {
          continue;
        }
        const LayoutConstraints constraints = layout.constraintsFor(view);
        minimum = std::max(minimum, vertical ? constraints.minHeight : constraints.minWidth);
      }
      return minimum;
    }

    int columnMaxPrimaryPx(const Column& column, const Layout& layout) {
      const bool vertical = layout.layoutConfig()->scrolling.direction == ScrollingDirection::Vertical;
      int maximum = 0;
      bool any = false;
      for (const View* view : column.views) {
        if (view == nullptr) {
          continue;
        }
        const LayoutConstraints constraints = layout.constraintsFor(view);
        const int clientMax = vertical ? constraints.maxHeight : constraints.maxWidth;
        if (clientMax > 0) {
          maximum = any ? std::min(maximum, clientMax) : clientMax;
          any = true;
        }
      }
      return any ? maximum : 0;
    }

    bool columnFillsViewport(const Column& column, const Layout& layout) {
      for (const View* view : column.views) {
        const LayoutConstraints constraints = layout.constraintsFor(view);
        if (view != nullptr && (constraints.fullscreen || constraints.maximizedToEdges)) {
          return true;
        }
      }
      return false;
    }

  } // namespace

  bool ScrollingLayout::vertical() const { return m_config->scrolling.direction == ScrollingDirection::Vertical; }

  bool ScrollingLayout::expandSingleColumn() const {
    return m_config != nullptr && m_config->scrolling.expandSingleColumn;
  }

  void ScrollingLayout::syncHeightWeights(Column& column) { ensureWeightCount(column); }

  int ScrollingLayout::columnOf(const View* view) const {
    for (size_t i = 0; i < m_columns.size(); ++i) {
      if (std::find(m_columns[i].views.begin(), m_columns[i].views.end(), view) != m_columns[i].views.end()) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  int ScrollingLayout::rowOf(const View* view) const {
    const int column = columnOf(view);
    if (column < 0) {
      return -1;
    }
    const auto& views = m_columns[static_cast<size_t>(column)].views;
    const auto it = std::ranges::find(views, view);
    return it == views.end() ? -1 : static_cast<int>(it - views.begin());
  }

  LayoutCapture ScrollingLayout::captureState() const { return captureStateForViewport(m_lastViewportPrimary); }

  LayoutCapture ScrollingLayout::captureStateForViewport(int viewportPrimary) const {
    auto snapshot = std::make_shared<ScrollingSnapshot>();
    LayoutCapture capture{.snapshot = snapshot, .members = {}};
    snapshot->columns.reserve(m_columns.size());
    for (size_t columnIndex = 0; columnIndex < m_columns.size(); ++columnIndex) {
      const Column& column = m_columns[columnIndex];
      ScrollingSnapshot::SavedColumn saved{
          .rows = {},
          .topGapWeight = column.topGapWeight,
          .bottomGapWeight = column.bottomGapWeight,
          .widthFraction = column.widthFrac,
          .savedWidthFraction = column.savedWidthFrac,
          .viewportCenterFraction = 0.5,
      };
      if (viewportPrimary > 0) {
        const double center = static_cast<double>(columnX(static_cast<int>(columnIndex), viewportPrimary))
            + static_cast<double>(columnWidth(static_cast<int>(columnIndex), viewportPrimary)) / 2.0;
        saved.viewportCenterFraction = (center - m_scroll) / static_cast<double>(viewportPrimary);
      }
      saved.rows.reserve(column.views.size());
      for (size_t row = 0; row < column.views.size(); ++row) {
        const auto id = static_cast<LayoutMemberId>(capture.members.size());
        capture.members.push_back({.id = id, .view = column.views[row]});
        saved.rows.push_back({
            .member = id,
            .heightWeight = row < column.heightWeights.size() ? column.heightWeights[row] : 1.0,
        });
      }
      snapshot->columns.push_back(std::move(saved));
    }
    snapshot->members = capture.members.size();
    snapshot->scroll = m_scroll;
    snapshot->centeredRest = m_centeredRest;
    snapshot->viewportPrimary = viewportPrimary;
    if (viewportPrimary > 0) {
      const auto maximum = static_cast<double>(maxScroll(viewportPrimary));
      snapshot->exactViewportRestorable = m_centeredRest || (m_scroll >= 0.0 && m_scroll <= maximum);
    }
    return capture;
  }

  bool ScrollingLayout::restoreState(const LayoutSnapshot& base, std::span<const LayoutMember> members) {
    m_pendingViewportSnapshot = nullptr;
    m_pendingViewportAnchor = nullptr;
    m_pendingViewportComplete = false;
    const auto* snapshot = dynamic_cast<const ScrollingSnapshot*>(&base);
    if (snapshot == nullptr || !m_columns.empty()) {
      return false;
    }
    const std::optional<std::vector<View*>> resolved = resolveLayoutMembers(snapshot->memberCount(), members);
    if (!resolved) {
      return false;
    }

    m_pendingViewportSnapshot = base.mode() == LayoutMode::Scrolling ? &base : nullptr;
    m_pendingViewportAnchor = nullptr;
    m_pendingViewportCenterFraction = 0.5;
    m_pendingViewportComplete = members.size() == snapshot->memberCount();
    double bestCenterDistance = 0.0;
    for (const ScrollingSnapshot::SavedColumn& saved : snapshot->columns) {
      Column column{
          .views = {},
          .heightWeights = {},
          .topGapWeight = saved.topGapWeight,
          .bottomGapWeight = saved.bottomGapWeight,
          .widthFrac = saved.widthFraction,
          .savedWidthFrac = saved.savedWidthFraction,
      };
      for (const ScrollingSnapshot::Row& row : saved.rows) {
        View* view = (*resolved)[static_cast<size_t>(row.member)];
        if (view != nullptr) {
          column.views.push_back(view);
          column.heightWeights.push_back(row.heightWeight);
        }
      }
      if (!column.views.empty()) {
        const double centerDistance = std::abs(saved.viewportCenterFraction - 0.5);
        if (m_pendingViewportAnchor == nullptr || centerDistance < bestCenterDistance) {
          m_pendingViewportAnchor = column.views.front();
          m_pendingViewportCenterFraction = saved.viewportCenterFraction;
          bestCenterDistance = centerDistance;
        }
        m_columns.push_back(std::move(column));
      }
    }
    m_targets.clear();
    if (m_pendingViewportComplete) {
      m_scroll = snapshot->scroll;
      m_centeredRest = snapshot->centeredRest;
    } else {
      m_scroll = 0.0;
      m_centeredRest = false;
    }
    m_lastAvailableCross = 0;
    return true;
  }

  void
  ScrollingLayout::restoreSnapshotViewport(const LayoutSnapshot& base, int viewportPrimary, bool geometryUnchanged) {
    const auto* snapshot = dynamic_cast<const ScrollingSnapshot*>(&base);
    if (snapshot == nullptr || m_pendingViewportSnapshot != &base) {
      return;
    }

    viewportPrimary = std::max(1, viewportPrimary);
    const int anchorColumn = columnOf(m_pendingViewportAnchor);
    bool columnGeometryMatches = m_columns.size() == snapshot->columns.size();
    if (columnGeometryMatches && snapshot->viewportPrimary == viewportPrimary) {
      for (size_t index = 0; index < m_columns.size(); ++index) {
        const double center = static_cast<double>(columnX(static_cast<int>(index), viewportPrimary))
            + static_cast<double>(columnWidth(static_cast<int>(index), viewportPrimary)) / 2.0;
        const double fraction = (center - snapshot->scroll) / static_cast<double>(viewportPrimary);
        if (std::abs(fraction - snapshot->columns[index].viewportCenterFraction) > 1e-9) {
          columnGeometryMatches = false;
          break;
        }
      }
    }
    const bool exact = geometryUnchanged
        && m_pendingViewportComplete
        && snapshot->exactViewportRestorable
        && snapshot->viewportPrimary == viewportPrimary
        && columnGeometryMatches;
    if (exact) {
      m_scroll = snapshot->scroll;
      m_centeredRest = snapshot->centeredRest;
    } else if (anchorColumn < 0) {
      m_scroll = 0.0;
      m_centeredRest = false;
    } else if (snapshot->centeredRest) {
      centerColumn(anchorColumn, viewportPrimary);
    } else {
      const double center = static_cast<double>(columnX(anchorColumn, viewportPrimary))
          + static_cast<double>(columnWidth(anchorColumn, viewportPrimary)) / 2.0;
      const double restored = center - m_pendingViewportCenterFraction * static_cast<double>(viewportPrimary);
      m_scroll = std::clamp(restored, 0.0, static_cast<double>(maxScroll(viewportPrimary)));
      m_centeredRest = false;
    }
    m_pendingViewportSnapshot = nullptr;
    m_pendingViewportAnchor = nullptr;
    m_pendingViewportComplete = false;
  }

  int ScrollingLayout::columnWidth(int columnIndex, int viewportPrimary) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return 0;
    }
    const Column& column = m_columns[static_cast<size_t>(columnIndex)];
    // Fullscreen lanes fill the entire viewport, bypassing widthFrac and size-hint clamps.
    if (columnFillsViewport(column, *this)) {
      const int edgePad = m_config->edgePad;
      return std::max(1, viewportPrimary + 2 * edgePad);
    }
    int width = 0;
    if (m_columns.size() == 1 && expandSingleColumn()) {
      // Fill the viewport without touching the stored fraction. Client size hints still apply to tiled columns.
      width = viewportPrimary;
    } else {
      // Gap-aware: reserve one inter-lane gap per lane so fractions summing to 1
      // tile exactly across the viewport primary extent.
      const int gap = m_config->totalGap;
      width = static_cast<int>(std::lround(column.widthFrac * (viewportPrimary + gap) - gap));
    }
    width = std::max(width, columnMinPrimaryPx(column, *this));
    const int maxWidth = columnMaxPrimaryPx(column, *this);
    if (maxWidth > 0) {
      width = std::min(width, maxWidth);
    }
    return std::clamp(width, 1, std::max(1, viewportPrimary));
  }

  bool ScrollingLayout::setWidthFromPixels(int columnIndex, int viewportPrimary, int width) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    const int gap = m_config->totalGap;
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    column.widthFrac =
        static_cast<double>(std::max(1, width) + gap) / static_cast<double>(std::max(1, viewportPrimary) + gap);
    column.savedWidthFrac = 0.0;
    return true;
  }

  int ScrollingLayout::centeringOffset(int viewportPrimary) const {
    if (!m_config->scrolling.centerUnderfullStrip) {
      return 0;
    }
    // When the tiled row is narrower than the viewport, split the leftover
    // space evenly on both sides.
    const int total = rawTotalWidth(viewportPrimary);
    if (total >= viewportPrimary) {
      return 0;
    }
    return (viewportPrimary - total) / 2;
  }

  int ScrollingLayout::columnX(int columnIndex, int viewportPrimary) const {
    const int end = std::clamp(columnIndex, 0, static_cast<int>(m_columns.size()));
    const int gap = m_config->totalGap;
    int x = centeringOffset(viewportPrimary);
    for (int i = 0; i < end; ++i) {
      x += columnWidth(i, viewportPrimary) + gap;
    }
    return x;
  }

  int ScrollingLayout::rawTotalWidth(int viewportPrimary) const {
    if (m_columns.empty()) {
      return 0;
    }
    const int gap = m_config->totalGap;
    int total = -gap;
    for (size_t i = 0; i < m_columns.size(); ++i) {
      total += columnWidth(static_cast<int>(i), viewportPrimary) + gap;
    }
    return std::max(0, total);
  }

  int ScrollingLayout::totalWidth(int viewportPrimary) const { return rawTotalWidth(viewportPrimary); }

  bool ScrollingLayout::isFullWidth(int columnIndex) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    const Column& column = m_columns[static_cast<size_t>(columnIndex)];
    return column.savedWidthFrac > 0.0 || columnFillsViewport(column, *this);
  }

  void ScrollingLayout::insertView(View* view, int columnIndex) {
    if (view == nullptr || columnOf(view) >= 0) {
      return;
    }
    const int index = std::clamp(columnIndex, 0, static_cast<int>(m_columns.size()));
    Column column;
    column.widthFrac = m_config->scrolling.defaultWidthFraction.value_or(0.5);
    column.views.push_back(view);
    column.heightWeights.push_back(1.0);
    m_columns.insert(m_columns.begin() + index, std::move(column));
  }

  void ScrollingLayout::insertViewIntoColumn(View* view, int columnIndex, int rowIndex) {
    if (view == nullptr
        || columnOf(view) >= 0
        || columnIndex < 0
        || columnIndex >= static_cast<int>(m_columns.size())) {
      return;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    ensureWeightCount(column);
    const int existingRows = static_cast<int>(column.views.size());
    const int row = std::clamp(rowIndex, 0, existingRows);

    double insertedWeight = 1.0;
    double edgeGapWeight = 0.0;
    bool consumesTopGap = false;
    bool consumesBottomGap = false;
    if (row == 0 && column.topGapWeight > 0.0) {
      edgeGapWeight = column.topGapWeight;
      consumesTopGap = true;
    } else if (row == existingRows && column.bottomGapWeight > 0.0) {
      edgeGapWeight = column.bottomGapWeight;
      consumesBottomGap = true;
    }

    if (edgeGapWeight > 0.0) {
      insertedWeight = edgeGapWeight;
      if (m_lastAvailableCross > 0) {
        const int gap = m_config->totalGap;
        const int oldStackHeight = std::max(existingRows, m_lastAvailableCross - std::max(0, existingRows - 1) * gap);
        const int newStackHeight = std::max(existingRows + 1, m_lastAvailableCross - existingRows * gap);
        const double oldTotalWeight = columnTotalWeight(column);
        const double unchangedWeight = oldTotalWeight - edgeGapWeight;
        const double newTotalWeight =
            oldTotalWeight * static_cast<double>(newStackHeight) / static_cast<double>(oldStackHeight);
        insertedWeight = std::max(kMinHeightWeight, newTotalWeight - unchangedWeight);
      }
      if (consumesTopGap) {
        column.topGapWeight = 0.0;
      }
      if (consumesBottomGap) {
        column.bottomGapWeight = 0.0;
      }
    }

    column.views.insert(column.views.begin() + row, view);
    column.heightWeights.insert(column.heightWeights.begin() + row, insertedWeight);
  }

  bool ScrollingLayout::consume(View* view, int direction) {
    const int sourceColumn = columnOf(view);
    const int destinationColumn = sourceColumn + direction;
    if ((direction != -1 && direction != 1)
        || sourceColumn < 0
        || destinationColumn < 0
        || destinationColumn >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& source = m_columns[static_cast<size_t>(sourceColumn)];
    Column& destination = m_columns[static_cast<size_t>(destinationColumn)];
    ensureWeightCount(source);
    ensureWeightCount(destination);
    const int row = rowOf(view);
    const double weight = row >= 0 ? source.heightWeights[static_cast<size_t>(row)] : 1.0;
    std::erase(source.views, view);
    if (row >= 0 && row < static_cast<int>(source.heightWeights.size())) {
      source.heightWeights.erase(source.heightWeights.begin() + row);
    }
    destination.views.push_back(view);
    destination.heightWeights.push_back(weight);
    if (source.views.empty()) {
      m_columns.erase(m_columns.begin() + sourceColumn);
    }
    return true;
  }

  bool ScrollingLayout::expel(View* view, int direction) {
    const int sourceColumn = columnOf(view);
    if ((direction != -1 && direction != 1) || sourceColumn < 0) {
      return false;
    }
    Column& source = m_columns[static_cast<size_t>(sourceColumn)];
    if (source.views.size() <= 1) {
      return false;
    }
    ensureWeightCount(source);
    const int row = rowOf(view);
    const double weight = row >= 0 ? source.heightWeights[static_cast<size_t>(row)] : 1.0;
    std::erase(source.views, view);
    if (row >= 0 && row < static_cast<int>(source.heightWeights.size())) {
      source.heightWeights.erase(source.heightWeights.begin() + row);
    }
    Column column;
    column.widthFrac = m_config->scrolling.defaultWidthFraction.value_or(0.5);
    column.views.push_back(view);
    column.heightWeights.push_back(weight);
    const int destinationColumn = sourceColumn + (direction > 0 ? 1 : 0);
    m_columns.insert(m_columns.begin() + destinationColumn, std::move(column));
    return true;
  }

  bool ScrollingLayout::moveViewVertical(View* view, int direction) {
    const int column = columnOf(view);
    const int row = rowOf(view);
    if (column < 0 || row < 0) {
      return false;
    }
    Column& col = m_columns[static_cast<size_t>(column)];
    ensureWeightCount(col);
    const int target = row + direction;
    if (target < 0 || target >= static_cast<int>(col.views.size())) {
      return false;
    }
    std::swap(col.views[static_cast<size_t>(row)], col.views[static_cast<size_t>(target)]);
    std::swap(col.heightWeights[static_cast<size_t>(row)], col.heightWeights[static_cast<size_t>(target)]);
    return true;
  }

  bool ScrollingLayout::swapViews(View* a, View* b) {
    if (a == b) {
      return false;
    }
    const int firstColumn = columnOf(a);
    const int firstRow = rowOf(a);
    const int secondColumn = columnOf(b);
    const int secondRow = rowOf(b);
    if (firstColumn < 0 || firstRow < 0 || secondColumn < 0 || secondRow < 0) {
      return false;
    }
    std::swap(
        m_columns[static_cast<size_t>(firstColumn)].views[static_cast<size_t>(firstRow)],
        m_columns[static_cast<size_t>(secondColumn)].views[static_cast<size_t>(secondRow)]
    );
    for (Target& target : m_targets) {
      if (target.view == a) {
        target.view = b;
      } else if (target.view == b) {
        target.view = a;
      }
    }
    return true;
  }

  void ScrollingLayout::removeView(View* view) {
    const int columnIndex = columnOf(view);
    if (columnIndex < 0) {
      return;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    ensureWeightCount(column);
    const int row = rowOf(view);
    std::erase(column.views, view);
    if (row >= 0 && row < static_cast<int>(column.heightWeights.size())) {
      column.heightWeights.erase(column.heightWeights.begin() + row);
    }
    if (column.views.empty()) {
      m_columns.erase(m_columns.begin() + columnIndex);
    }
    std::erase_if(m_targets, [view](const Target& target) { return target.view == view; });
  }

  void ScrollingLayout::moveColumn(int from, int to) {
    if (from < 0 || from >= static_cast<int>(m_columns.size())) {
      return;
    }
    const int destination = std::clamp(to, 0, static_cast<int>(m_columns.size()) - 1);
    if (from == destination) {
      return;
    }
    Column column = std::move(m_columns[static_cast<size_t>(from)]);
    m_columns.erase(m_columns.begin() + from);
    m_columns.insert(m_columns.begin() + destination, std::move(column));
  }

  void ScrollingLayout::setScroll(double scroll, bool centeredRest) {
    m_scroll = scroll;
    m_centeredRest = centeredRest;
  }

  bool ScrollingLayout::centerColumn(int columnIndex, int viewportPrimary) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size()) || viewportPrimary <= 0) {
      return false;
    }
    const double target = static_cast<double>(columnX(columnIndex, viewportPrimary))
        - (viewportPrimary - columnWidth(columnIndex, viewportPrimary)) / 2.0;
    setScroll(target, true);
    return true;
  }

  void ScrollingLayout::reconcileFocusedColumn(int columnIndex, int viewportPrimary) {
    if (m_config->scrolling.centerFocused) {
      centerColumn(columnIndex, viewportPrimary);
      return;
    }
    m_centeredRest = false;
    ensureVisible(columnIndex, viewportPrimary);
  }

  double ScrollingLayout::targetScrollForEnsureVisible(int columnIndex, int viewportPrimary, bool force) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size()) || viewportPrimary <= 0) {
      return m_scroll;
    }
    const int x = columnX(columnIndex, viewportPrimary);
    const int width = columnWidth(columnIndex, viewportPrimary);
    const double max = static_cast<double>(std::max(0, totalWidth(viewportPrimary) - viewportPrimary));

    if (width >= viewportPrimary) {
      const double cover = static_cast<double>(x) + static_cast<double>(width - viewportPrimary) / 2.0;
      return std::clamp(cover, 0.0, max);
    }
    if (m_config->scrolling.centerFocused) {
      return static_cast<double>(x) - (viewportPrimary - width) / 2.0;
    }
    if (force) {
      return std::clamp(static_cast<double>(x), 0.0, max);
    }
    // Already fully on screen: never move the strip, including one parked past an edge on purpose (column-center
    // overshoots the range so edge columns can sit in the middle). A touchpad swipe that left the strip outside its
    // range springs back where the gesture ends, in Gestures::finishScroll.
    if (m_scroll <= static_cast<double>(x) && m_scroll >= static_cast<double>(x + width - viewportPrimary)) {
      return m_centeredRest ? m_scroll : std::clamp(m_scroll, 0.0, max);
    }

    // Move by the shortest distance that reveals the whole column. A column entering from the right lands flush against
    // the right edge, while one entering from the left lands flush against the left edge. Do not reserve space for a
    // neighboring sliver after focus has moved.
    const double scroll =
        std::clamp(m_scroll, static_cast<double>(x + width - viewportPrimary), static_cast<double>(x));
    return std::clamp(scroll, 0.0, max);
  }

  double ScrollingLayout::scrollAmountToEnsureVisible(int columnIndex, int viewportPrimary) const {
    if (viewportPrimary <= 0) {
      return 0.0;
    }
    return std::abs(targetScrollForEnsureVisible(columnIndex, viewportPrimary) - m_scroll)
        / static_cast<double>(viewportPrimary);
  }

  double ScrollingLayout::scrollShiftForColumnRemoval(int columnIndex, int viewportPrimary) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size()) || viewportPrimary <= 0) {
      return 0.0;
    }
    // A stack losing one row leaves the horizontal geometry alone; only the
    // last view out takes the column, and the space, with it.
    if (m_columns[static_cast<size_t>(columnIndex)].views.size() != 1) {
      return 0.0;
    }
    const double span = static_cast<double>(columnWidth(columnIndex, viewportPrimary)) + m_config->totalGap;
    const double hidden = m_scroll - static_cast<double>(columnX(columnIndex, viewportPrimary));
    return std::clamp(hidden, 0.0, span);
  }

  void ScrollingLayout::ensureVisible(int columnIndex, int viewportPrimary) {
    const double target = targetScrollForEnsureVisible(columnIndex, viewportPrimary, false);
    m_centeredRest = m_config->scrolling.centerFocused || (m_centeredRest && target == m_scroll);
    m_scroll = target;
  }

  void ScrollingLayout::snapVisible(int columnIndex, int viewportPrimary) {
    m_centeredRest = m_config->scrolling.centerFocused;
    m_scroll = targetScrollForEnsureVisible(columnIndex, viewportPrimary, true);
  }

  void ScrollingLayout::arrange(const wlr_box& usable) {
    m_targets.clear();
    // Include outer border in the usable-area inset so decorations stay clear of
    // layer-shell exclusive zones (panels).
    const bool v = vertical();
    const int edgePad = m_config->edgePad;
    const int viewportPrimary = std::max(1, (v ? usable.height : usable.width) - 2 * edgePad);
    m_lastViewportPrimary = viewportPrimary;
    const int availableCross = std::max(1, (v ? usable.width : usable.height) - 2 * edgePad);
    m_lastAvailableCross = availableCross;
    const double maxScroll = static_cast<double>(std::max(0, totalWidth(viewportPrimary) - viewportPrimary));
    // Allow overscroll past both strip edges so gesture spring-back is visible.
    const auto viewport = static_cast<double>(viewportPrimary);
    m_scroll = std::clamp(m_scroll, -viewport, maxScroll + viewport);

    const int gap = m_config->totalGap;
    // columnX() re-sums every prior column each call; keep a running x instead of calling it per column.
    int runningColumnX = centeringOffset(viewportPrimary);
    for (size_t columnIndex = 0; columnIndex < m_columns.size(); ++columnIndex) {
      Column& column = m_columns[columnIndex];
      const int primarySize = columnWidth(static_cast<int>(columnIndex), viewportPrimary);
      if (column.views.empty()) {
        runningColumnX += primarySize + gap;
        continue;
      }
      ensureWeightCount(column);
      const int primary =
          (v ? usable.y : usable.x) + edgePad + runningColumnX - static_cast<int>(std::lround(m_scroll));
      runningColumnX += primarySize + gap;
      const int rowCount = static_cast<int>(column.views.size());
      const int gapsTotal = std::max(0, rowCount - 1) * gap;
      const int stackCross = std::max(rowCount, availableCross - gapsTotal);
      const double totalWeight = columnTotalWeight(column);

      int cross = (v ? usable.x : usable.y) + edgePad;
      const int startGapPx =
          static_cast<int>(std::lround(std::max(0.0, column.topGapWeight) / totalWeight * stackCross));
      cross += startGapPx;
      int used = startGapPx;

      for (int row = 0; row < rowCount; ++row) {
        const double weight = std::max(kMinHeightWeight, column.heightWeights[static_cast<size_t>(row)]);
        int crossSize = static_cast<int>(std::lround(weight / totalWeight * stackCross));
        if (row == rowCount - 1) {
          const int endGapPx =
              static_cast<int>(std::lround(std::max(0.0, column.bottomGapWeight) / totalWeight * stackCross));
          crossSize = std::max(1, stackCross - used - endGapPx);
        } else {
          crossSize = std::max(1, crossSize);
        }
        View* view = column.views[static_cast<size_t>(row)];
        if (view != nullptr) {
          const LayoutConstraints constraints = constraintsFor(view);
          crossSize = v ? constraints.clampWidth(crossSize) : constraints.clampHeight(crossSize);
        }
        if (v) {
          m_targets.push_back({.view = view, .x = cross, .y = primary, .width = crossSize, .height = primarySize});
        } else {
          m_targets.push_back({.view = view, .x = primary, .y = cross, .width = primarySize, .height = crossSize});
        }
        cross += crossSize + gap;
        used += crossSize;
      }
    }
  }

  Layout::InitialSize ScrollingLayout::initialSize(
      const wlr_box& usable, std::optional<double> ruleWidthFraction, const View* /*splitAnchor*/
  ) const {
    const wlr_box content = contentArea(usable);
    // The first window of a lone-column workspace opens full so its first buffer matches what arrange() will assign.
    if (m_columns.empty() && expandSingleColumn()) {
      return {.width = content.width, .height = content.height};
    }
    const std::optional<double> fraction =
        ruleWidthFraction ? ruleWidthFraction : m_config->scrolling.defaultWidthFraction;
    if (!fraction) {
      return vertical() ? InitialSize{.width = content.width, .height = 0}
                        : InitialSize{.width = 0, .height = content.height};
    }
    if (vertical()) {
      return {.width = content.width, .height = fractionalWidth(content.height, *fraction)};
    }
    return {.width = fractionalWidth(content.width, *fraction), .height = content.height};
  }

  wlr_box ScrollingLayout::targetBox(const View* view) const {
    const auto it = std::ranges::find_if(m_targets, [view](const Target& target) { return target.view == view; });
    if (it == m_targets.end()) {
      return {};
    }
    return {.x = it->x, .y = it->y, .width = it->width, .height = it->height};
  }

  bool ScrollingLayout::cycleWidth(int columnIndex, int direction) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    column.widthFrac = nextFractionPreset(m_config->widthPresets, column.widthFrac, direction);
    column.savedWidthFrac = 0.0;
    return true;
  }

  bool ScrollingLayout::toggleFullWidth(int columnIndex) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    if (column.savedWidthFrac > 0.0) {
      column.widthFrac = column.savedWidthFrac;
      column.savedWidthFrac = 0.0;
    } else {
      column.savedWidthFrac = column.widthFrac;
      column.widthFrac = 1.0;
    }
    return column.savedWidthFrac > 0.0;
  }

  bool ScrollingLayout::setWidthFraction(int columnIndex, double fraction) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    column.widthFrac = std::clamp(fraction, 0.1, 1.0);
    column.savedWidthFrac = 0.0;
    return true;
  }

  void ScrollingLayout::clearFullWidthState(int columnIndex) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return;
    }
    m_columns[static_cast<size_t>(columnIndex)].savedWidthFrac = 0.0;
  }

  double ScrollingLayout::widthFraction(int columnIndex) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return m_config->scrolling.defaultWidthFraction.value_or(0.5);
    }
    return m_columns[static_cast<size_t>(columnIndex)].widthFrac;
  }

  double ScrollingLayout::heightFraction(const View* view) const {
    const int columnIndex = columnOf(view);
    if (columnIndex < 0) {
      return 1.0;
    }
    const Column& column = m_columns[static_cast<size_t>(columnIndex)];
    if (column.views.size() <= 1) {
      return 1.0;
    }
    const int row = rowOf(view);
    return std::max(kMinHeightWeight, heightWeight(columnIndex, row)) / columnTotalWeight(column);
  }

  bool ScrollingLayout::setHeightFraction(View* view, double fraction) {
    const int columnIndex = columnOf(view);
    if (columnIndex < 0) {
      return false;
    }
    const Column& column = m_columns[static_cast<size_t>(columnIndex)];
    if (column.views.size() <= 1) {
      return false;
    }
    const int row = rowOf(view);
    const double oldWeight = std::max(kMinHeightWeight, heightWeight(columnIndex, row));
    const double others = columnTotalWeight(column) - oldWeight;
    const double target = std::clamp(fraction, 0.1, 0.95);
    return setHeightWeight(columnIndex, row, target * others / (1.0 - target));
  }

  bool ScrollingLayout::setRowBoundary(int columnIndex, int upperRow, double upperWeight, double lowerWeight) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    ensureWeightCount(column);
    if (upperRow < 0 || upperRow + 1 >= static_cast<int>(column.views.size())) {
      return false;
    }
    column.heightWeights[static_cast<size_t>(upperRow)] = std::max(kMinHeightWeight, upperWeight);
    column.heightWeights[static_cast<size_t>(upperRow + 1)] = std::max(kMinHeightWeight, lowerWeight);
    return true;
  }

  bool ScrollingLayout::setHeightWeight(int columnIndex, int row, double weight) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    ensureWeightCount(column);
    if (row < 0 || row >= static_cast<int>(column.heightWeights.size())) {
      return false;
    }
    column.heightWeights[static_cast<size_t>(row)] = std::max(kMinHeightWeight, weight);
    return true;
  }

  bool ScrollingLayout::setTopGapWeight(int columnIndex, double weight) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    m_columns[static_cast<size_t>(columnIndex)].topGapWeight = std::max(0.0, weight);
    return true;
  }

  bool ScrollingLayout::setBottomGapWeight(int columnIndex, double weight) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    m_columns[static_cast<size_t>(columnIndex)].bottomGapWeight = std::max(0.0, weight);
    return true;
  }

  double ScrollingLayout::heightWeight(int columnIndex, int row) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return 1.0;
    }
    const Column& column = m_columns[static_cast<size_t>(columnIndex)];
    if (row < 0 || row >= static_cast<int>(column.heightWeights.size())) {
      return 1.0;
    }
    return column.heightWeights[static_cast<size_t>(row)];
  }

  double ScrollingLayout::topGapWeight(int columnIndex) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return 0.0;
    }
    return m_columns[static_cast<size_t>(columnIndex)].topGapWeight;
  }

  double ScrollingLayout::bottomGapWeight(int columnIndex) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return 0.0;
    }
    return m_columns[static_cast<size_t>(columnIndex)].bottomGapWeight;
  }

  // Interactive resize
  namespace {

    // Layout-owned resize session. Holds the state captured at grab start and
    // reproduces the width-fraction / row-weight math from a pointer delta.
    class ScrollingResizeGrab : public ResizeGrab {
    public:
      ScrollingResizeGrab(
          ScrollingLayout* layout, int column, int row, uint32_t edges, bool vertical, bool soloPrimary,
          bool clearedFullWidth, double startScroll, int startColumnX, int startPrimaryPx, int startPrevPrimaryPx,
          int startStripPrimaryPx, int upperRow, double startUpperWeight, double startLowerWeight
      )
          : m_layout(layout), m_column(column), m_row(row), m_edges(edges), m_vertical(vertical),
            m_soloPrimary(soloPrimary), m_clearedFullWidth(clearedFullWidth), m_startScroll(startScroll),
            m_startColumnX(startColumnX), m_startPrimaryPx(startPrimaryPx), m_startPrevPrimaryPx(startPrevPrimaryPx),
            m_startStripPrimaryPx(startStripPrimaryPx), m_upperRow(upperRow), m_startUpperWeight(startUpperWeight),
            m_startLowerWeight(startLowerWeight) {}

      [[nodiscard]] bool unmaximizeOnBegin() const override { return m_clearedFullWidth; }

      [[nodiscard]] const Layout* ownerLayout() const override { return m_layout; }

      void applyDelta(double dx, double dy, const wlr_box& usable) override {
        ScrollingLayout& layout = *m_layout;
        if (m_column < 0 || m_column >= static_cast<int>(layout.columns().size())) {
          return;
        }
        const double dPrimary = m_vertical ? dy : dx;
        const double dCross = m_vertical ? dx : dy;
        const uint32_t primaryStartEdge = m_vertical ? WLR_EDGE_TOP : WLR_EDGE_LEFT;
        const uint32_t primaryEndEdge = m_vertical ? WLR_EDGE_BOTTOM : WLR_EDGE_RIGHT;
        const uint32_t crossStartEdge = m_vertical ? WLR_EDGE_LEFT : WLR_EDGE_TOP;
        const uint32_t crossEndEdge = m_vertical ? WLR_EDGE_RIGHT : WLR_EDGE_BOTTOM;

        // Recompute viewport metrics live each motion so output changes are reflected.
        // Direction itself remains fixed to the value captured at grab start.
        const int edgePad = layout.layoutConfig()->edgePad;
        const int viewportPrimary = std::max(1, (m_vertical ? usable.height : usable.width) - 2 * edgePad);
        const int availableCross = std::max(1, (m_vertical ? usable.width : usable.height) - 2 * edgePad);

        auto columnMinPrimary = [&](int columnIndex) {
          int minimum = static_cast<int>(std::lround(0.15 * viewportPrimary));
          if (columnIndex < 0 || columnIndex >= static_cast<int>(layout.columns().size())) {
            return minimum;
          }
          for (View* view : layout.columns()[static_cast<size_t>(columnIndex)].views) {
            if (view != nullptr) {
              const LayoutConstraints constraints = layout.constraintsFor(view);
              minimum = std::max(minimum, m_vertical ? constraints.minHeight : constraints.minWidth);
            }
          }
          return std::min(minimum, viewportPrimary);
        };
        auto columnMaxPrimary = [&](int columnIndex) {
          int maximum = viewportPrimary;
          if (columnIndex < 0 || columnIndex >= static_cast<int>(layout.columns().size())) {
            return maximum;
          }
          bool any = false;
          int clientMax = 0;
          for (View* view : layout.columns()[static_cast<size_t>(columnIndex)].views) {
            if (view == nullptr) {
              continue;
            }
            const LayoutConstraints constraints = layout.constraintsFor(view);
            const int hintMax = m_vertical ? constraints.maxHeight : constraints.maxWidth;
            if (hintMax > 0) {
              clientMax = any ? std::min(clientMax, hintMax) : hintMax;
              any = true;
            }
          }
          if (any) {
            maximum = std::min(maximum, clientMax);
          }
          return std::max(maximum, columnMinPrimary(columnIndex));
        };
        const int gap = layout.layoutConfig()->totalGap;
        auto setColumnPrimaryPx = [&](int columnIndex, int extent) {
          const double fraction = static_cast<double>(extent + gap) / static_cast<double>(viewportPrimary + gap);
          layout.setWidthFraction(columnIndex, fraction);
        };
        const bool centerUnderfullStrip =
            m_startStripPrimaryPx < viewportPrimary && layout.layoutConfig()->scrolling.centerUnderfullStrip;
        const double centeredEdgeTravel =
            std::max(0.0, static_cast<double>(viewportPrimary - m_startStripPrimaryPx) / 2.0);
        auto centeredPrimaryDelta = [centeredEdgeTravel](double edgeDelta) {
          return edgeDelta <= centeredEdgeTravel ? 2.0 * edgeDelta : edgeDelta + centeredEdgeTravel;
        };

        if ((m_edges & primaryEndEdge) != 0) {
          // While the strip fits, its center stays fixed. A boundary extent changes
          // by two pointer pixels for each pixel the centered outer edge moves.
          const double extentDelta = centerUnderfullStrip ? centeredPrimaryDelta(dPrimary) : dPrimary;
          const int newExtent = std::clamp(
              m_startPrimaryPx + static_cast<int>(std::lround(extentDelta)), columnMinPrimary(m_column),
              columnMaxPrimary(m_column)
          );
          setColumnPrimaryPx(m_column, newExtent);
          if (centerUnderfullStrip) {
            layout.setScroll(0.0);
          } else {
            layout.setScroll(
                m_startScroll + static_cast<double>(layout.columnX(m_column, viewportPrimary) - m_startColumnX)
            );
          }
        } else if ((m_edges & primaryStartEdge) != 0) {
          if (m_column > 0 && !m_soloPrimary) {
            // Shared boundary with the previous lane: keep the pair span fixed.
            const int pair = m_startPrevPrimaryPx + gap + m_startPrimaryPx;
            const int minPrev = columnMinPrimary(m_column - 1);
            const int minCur = columnMinPrimary(m_column);
            const int maxPrev = columnMaxPrimary(m_column - 1);
            const int maxCur = columnMaxPrimary(m_column);
            const int newPrev = std::clamp(
                m_startPrevPrimaryPx + static_cast<int>(std::lround(dPrimary)), std::max(minPrev, pair - gap - maxCur),
                std::min(maxPrev, pair - gap - minCur)
            );
            const int newCur = pair - gap - newPrev;
            setColumnPrimaryPx(m_column - 1, newPrev);
            setColumnPrimaryPx(m_column, newCur);
          } else {
            const double extentDelta = centerUnderfullStrip ? centeredPrimaryDelta(-dPrimary) : -dPrimary;
            const int newExtent = std::clamp(
                m_startPrimaryPx + static_cast<int>(std::lround(extentDelta)), columnMinPrimary(m_column),
                columnMaxPrimary(m_column)
            );
            setColumnPrimaryPx(m_column, newExtent);
            if (centerUnderfullStrip) {
              layout.setScroll(0.0);
            } else {
              layout.setScroll(
                  m_startScroll
                  + static_cast<double>(layout.columnX(m_column, viewportPrimary) - m_startColumnX)
                  + static_cast<double>(newExtent - m_startPrimaryPx)
              );
            }
          }
        }

        if ((m_edges & (crossStartEdge | crossEndEdge)) != 0 && m_row >= 0) {
          const Column& column = layout.columns()[static_cast<size_t>(m_column)];
          const int rowCount = static_cast<int>(column.views.size());
          const int gapsTotal = std::max(0, rowCount - 1) * gap;
          const int stackCross = std::max(rowCount, availableCross - gapsTotal);
          if (stackCross > 0) {
            constexpr double kMinWindow = 0.05;
            double totalWeight = std::max(0.0, column.topGapWeight) + std::max(0.0, column.bottomGapWeight);
            for (double weight : column.heightWeights) {
              totalWeight += std::max(kMinWindow, weight);
            }
            totalWeight = std::max(kMinWindow, totalWeight);

            auto minWindowWeight = [&](View* view) {
              if (view == nullptr) {
                return kMinWindow;
              }
              const LayoutConstraints constraints = layout.constraintsFor(view);
              const int minimum = m_vertical ? constraints.minWidth : constraints.minHeight;
              return std::max(kMinWindow, static_cast<double>(minimum) / stackCross * totalWeight);
            };

            const double pair = std::max(kMinWindow, m_startUpperWeight + m_startLowerWeight);
            const double deltaWeight = dCross / static_cast<double>(stackCross) * totalWeight;

            auto splitWindows = [&](double startUpper, double /*startLower*/, double delta, double minUpper,
                                    double minLower) {
              double upper = startUpper + delta;
              double lower = pair - upper;
              if (upper < minUpper) {
                upper = minUpper;
                lower = pair - upper;
              }
              if (lower < minLower) {
                lower = minLower;
                upper = pair - lower;
              }
              return std::pair{upper, lower};
            };
            auto splitGapAndWindow = [&](double startGap, double /*startWindow*/, double deltaGap, double minWindow) {
              double gapWeight = startGap + deltaGap;
              double windowWeight = pair - gapWeight;
              if (gapWeight < 0.0) {
                gapWeight = 0.0;
                windowWeight = pair;
              }
              if (windowWeight < minWindow) {
                windowWeight = minWindow;
                gapWeight = pair - windowWeight;
                if (gapWeight < 0.0) {
                  gapWeight = 0.0;
                  windowWeight = pair;
                }
              }
              return std::pair{gapWeight, windowWeight};
            };

            // "Upper" denotes the cross-start side, which is left when vertical.
            if ((m_edges & crossStartEdge) != 0) {
              if (m_row == 0) {
                const double minWindow = minWindowWeight(column.views[0]);
                const auto [gapWeight, windowWeight] =
                    splitGapAndWindow(m_startUpperWeight, m_startLowerWeight, deltaWeight, minWindow);
                layout.setTopGapWeight(m_column, gapWeight);
                layout.setHeightWeight(m_column, 0, windowWeight);
              } else if (m_upperRow >= 0) {
                const double minUpper = minWindowWeight(column.views[static_cast<size_t>(m_upperRow)]);
                const double minLower = minWindowWeight(column.views[static_cast<size_t>(m_row)]);
                const auto [upper, lower] =
                    splitWindows(m_startUpperWeight, m_startLowerWeight, deltaWeight, minUpper, minLower);
                layout.setRowBoundary(m_column, m_upperRow, upper, lower);
              }
            } else if ((m_edges & crossEndEdge) != 0) {
              if (m_row + 1 >= rowCount) {
                const double minWindow = minWindowWeight(column.views[static_cast<size_t>(m_row)]);
                const auto [gapWeight, windowWeight] =
                    splitGapAndWindow(m_startLowerWeight, m_startUpperWeight, -deltaWeight, minWindow);
                layout.setHeightWeight(m_column, m_row, windowWeight);
                layout.setBottomGapWeight(m_column, gapWeight);
              } else if (m_upperRow >= 0) {
                const double minUpper = minWindowWeight(column.views[static_cast<size_t>(m_upperRow)]);
                const double minLower = minWindowWeight(column.views[static_cast<size_t>(m_row + 1)]);
                const auto [upper, lower] =
                    splitWindows(m_startUpperWeight, m_startLowerWeight, deltaWeight, minUpper, minLower);
                layout.setRowBoundary(m_column, m_upperRow, upper, lower);
              }
            }
          }
        }
      }

    private:
      ScrollingLayout* m_layout;
      int m_column;
      int m_row;
      uint32_t m_edges;
      bool m_vertical;
      bool m_soloPrimary;
      bool m_clearedFullWidth;
      double m_startScroll;
      int m_startColumnX;
      int m_startPrimaryPx;
      int m_startPrevPrimaryPx;
      int m_startStripPrimaryPx;
      int m_upperRow;
      double m_startUpperWeight;
      double m_startLowerWeight;
    };

  } // namespace

  uint32_t ScrollingLayout::resizeEdgesAt(const View* view, double cx, double cy) const {
    const wlr_box box = targetBox(view);
    if (box.width <= 0 || box.height <= 0) {
      return WLR_EDGE_RIGHT;
    }
    return sanitizeResizeEdges(view, resizeEdgesForPoint(box, cx, cy));
  }

  uint32_t ScrollingLayout::sanitizeResizeEdges(const View* view, uint32_t edges) const {
    if (columnOf(view) == 0 && !m_config->scrolling.centerUnderfullStrip) {
      edges &= ~(vertical() ? WLR_EDGE_TOP : WLR_EDGE_LEFT);
    }
    return edges;
  }

  std::unique_ptr<ResizeGrab> ScrollingLayout::beginResize(View* view, uint32_t edges, const wlr_box& usable) {
    const int column = columnOf(view);
    if (column < 0) {
      return nullptr;
    }
    const int row = rowOf(view);
    const bool v = vertical();
    const uint32_t crossStartEdge = v ? WLR_EDGE_LEFT : WLR_EDGE_TOP;
    const uint32_t crossEndEdge = v ? WLR_EDGE_RIGHT : WLR_EDGE_BOTTOM;

    bool soloPrimary = false;
    bool clearedFullWidth = false;
    if (isFullWidth(column)) {
      clearFullWidthState(column);
      soloPrimary = true;
      clearedFullWidth = true;
    }

    const int viewportPrimary = std::max(1, (v ? usable.height : usable.width) - 2 * m_config->edgePad);

    const wlr_box box = targetBox(view);
    const int startColumnX = columnX(column, viewportPrimary);
    const int startPrimaryPx = v ? box.height : box.width;
    if (startPrimaryPx >= viewportPrimary) {
      soloPrimary = true;
    }
    int startPrevPrimaryPx = 0;
    if (column > 0 && !soloPrimary) {
      startPrevPrimaryPx = columnWidth(column - 1, viewportPrimary);
    }
    const double startScroll = scroll();

    int upperRow = -1;
    double startUpperWeight = 0;
    double startLowerWeight = 0;
    if ((edges & (crossStartEdge | crossEndEdge)) != 0 && row >= 0) {
      const Column& lane = m_columns[static_cast<size_t>(column)];
      if ((edges & crossStartEdge) != 0) {
        if (row > 0) {
          upperRow = row - 1;
          startUpperWeight = heightWeight(column, upperRow);
          startLowerWeight = heightWeight(column, row);
        } else {
          startUpperWeight = topGapWeight(column);
          startLowerWeight = heightWeight(column, 0);
        }
      } else if ((edges & crossEndEdge) != 0) {
        if (row + 1 < static_cast<int>(lane.views.size())) {
          upperRow = row;
          startUpperWeight = heightWeight(column, row);
          startLowerWeight = heightWeight(column, row + 1);
        } else {
          upperRow = static_cast<int>(lane.views.size()) - 1;
          startUpperWeight = heightWeight(column, row);
          startLowerWeight = bottomGapWeight(column);
        }
      }
    }

    return std::make_unique<ScrollingResizeGrab>(
        this, column, row, edges, v, soloPrimary, clearedFullWidth, startScroll, startColumnX, startPrimaryPx,
        startPrevPrimaryPx, rawTotalWidth(viewportPrimary), upperRow, startUpperWeight, startLowerWeight
    );
  }

} // namespace umbriel
