#include "layout/scrolling.h"

#include "config/config.h"

#include <algorithm>
#include <cmath>
#include <utility>

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
        ColumnTabs tabs;
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

    void ensureRememberedExtentCount(Column& column) {
      while (column.rememberedScrollingExtents.size() < column.views.size()) {
        column.rememberedScrollingExtents.push_back(0.0);
      }
      if (column.rememberedScrollingExtents.size() > column.views.size()) {
        column.rememberedScrollingExtents.resize(column.views.size());
      }
    }

    // A column's per-row vectors change together, and its tab selection follows its view through every change.
    struct ErasedRow {
      double heightWeight = 1.0;
      double rememberedExtent = 0.0;
    };

    // A row joining a tab group, strictly inside one or into group `into`, takes the group's shared extent rather than
    // `heightWeight`.
    void insertRow(
        Column& column, size_t row, View* view, double heightWeight, double rememberedExtent,
        std::optional<size_t> into = std::nullopt
    ) {
      ensureWeightCount(column);
      ensureRememberedExtentCount(column);
      row = std::min(row, column.views.size());
      const auto at = static_cast<std::ptrdiff_t>(row);
      column.views.insert(column.views.begin() + at, view);
      column.heightWeights.insert(column.heightWeights.begin() + at, heightWeight);
      column.rememberedScrollingExtents.insert(column.rememberedScrollingExtents.begin() + at, rememberedExtent);
      column.tabs.rowInserted(row, into);
      if (const TabGroup* group = column.tabs.groupAt(row); group != nullptr && group->count > 1) {
        column.heightWeights[row] = column.heightWeights[row == group->first ? row + 1 : group->first];
      }
    }

    ErasedRow eraseRow(Column& column, size_t row) {
      ensureWeightCount(column);
      ensureRememberedExtentCount(column);
      const auto at = static_cast<std::ptrdiff_t>(row);
      const ErasedRow erased{
          .heightWeight = column.heightWeights[row],
          .rememberedExtent = column.rememberedScrollingExtents[row],
      };
      column.views.erase(column.views.begin() + at);
      column.heightWeights.erase(column.heightWeights.begin() + at);
      column.rememberedScrollingExtents.erase(column.rememberedScrollingExtents.begin() + at);
      column.tabs.rowErased(row);
      return erased;
    }

    void swapRows(Column& column, size_t first, size_t second) {
      ensureWeightCount(column);
      ensureRememberedExtentCount(column);
      std::swap(column.views[first], column.views[second]);
      std::swap(column.heightWeights[first], column.heightWeights[second]);
      std::swap(column.rememberedScrollingExtents[first], column.rememberedScrollingExtents[second]);
      column.tabs.rowsSwapped(first, second);
    }

    // A tab group is one unit of the stack, so its shared weight counts once.
    double columnTotalWeight(const Column& column) {
      double total = std::max(0.0, column.topGapWeight) + std::max(0.0, column.bottomGapWeight);
      for (size_t row = 0; row < column.heightWeights.size(); row = column.tabs.unitEnd(row)) {
        total += std::max(kMinHeightWeight, column.heightWeights[row]);
      }
      return std::max(kMinHeightWeight, total);
    }

    // Every row of the unit holding `row` takes `weight`: the tabs of a group share one extent.
    void setUnitWeight(Column& column, size_t row, double weight) {
      ensureWeightCount(column);
      const size_t end = std::min(column.tabs.unitEnd(row), column.heightWeights.size());
      for (size_t member = column.tabs.unitStart(row); member < end; ++member) {
        column.heightWeights[member] = weight;
      }
    }

    // The heaviest minimum extent on the stacking axis among the rows of the unit holding `row`.
    int unitMinCross(const Column& column, size_t row, const Layout& layout, bool vertical) {
      int minimum = 0;
      const size_t end = std::min(column.tabs.unitEnd(row), column.views.size());
      for (size_t member = column.tabs.unitStart(row); member < end; ++member) {
        if (const View* view = column.views[member]) {
          const LayoutConstraints constraints = layout.constraintsFor(view);
          minimum = std::max(minimum, vertical ? constraints.minWidth : constraints.minHeight);
        }
      }
      return minimum;
    }

    // Rotate rows [first, last) of every per-row vector so row `middle` comes first. Tab groups are the caller's to
    // follow.
    void rotateRows(Column& column, size_t first, size_t middle, size_t last) {
      ensureWeightCount(column);
      ensureRememberedExtentCount(column);
      const auto rotate = [&](auto& rows) {
        std::rotate(
            rows.begin() + static_cast<std::ptrdiff_t>(first), rows.begin() + static_cast<std::ptrdiff_t>(middle),
            rows.begin() + static_cast<std::ptrdiff_t>(last)
        );
      };
      rotate(column.views);
      rotate(column.heightWeights);
      rotate(column.rememberedScrollingExtents);
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

    // Fullscreen and maximize-to-edges views are presented against the wider
    // usable area, so they bleed one strut past each end of the strut-inset
    // strip. The strip reserves that overhang, otherwise the neighboring column
    // lands underneath the window. Fullscreen is presented against the whole
    // output, so only its strut share is reserved and it still bleeds over a
    // layer-shell exclusive zone.
    struct ColumnBleed {
      int before = 0;
      int after = 0;
    };

    ColumnBleed columnBleed(const Column& column, const Layout& layout) {
      if (!columnFillsViewport(column, layout)) {
        return {};
      }
      const LayoutStruts& struts = layout.layoutConfig()->struts;
      const bool vertical = layout.layoutConfig()->scrolling.direction == ScrollingDirection::Vertical;
      return vertical ? ColumnBleed{.before = struts.top, .after = struts.bottom}
                      : ColumnBleed{.before = struts.left, .after = struts.right};
    }

  } // namespace

  bool ScrollingLayout::vertical() const { return m_config->scrolling.direction == ScrollingDirection::Vertical; }

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
          .tabs = column.tabs,
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
          .rememberedScrollingExtents = {},
          .tabs = saved.tabs,
      };
      for (const ScrollingSnapshot::Row& row : saved.rows) {
        View* view = (*resolved)[static_cast<size_t>(row.member)];
        if (view != nullptr) {
          column.views.push_back(view);
          column.heightWeights.push_back(row.heightWeight);
          column.rememberedScrollingExtents.push_back(0.0);
        } else {
          // A member that did not come back leaves its row the way a close does.
          column.tabs.rowErased(column.views.size());
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
    m_lastFocusedColumn = -1;
    m_focusSide = FocusSide::None;
    m_removedFocusedColumn = -1;
    m_pendingRemovalReevaluate = false;
    m_policyCenteredRest = false;
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
    m_policyCenteredRest = false;
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
    const int gap = m_config->totalGap;
    const auto slotExtent = static_cast<double>(viewportPrimary + gap);
    double slotStart = 0.0;
    for (int i = 0; i < columnIndex; ++i) {
      slotStart += m_columns[static_cast<size_t>(i)].widthFrac * slotExtent;
    }
    const double slotEnd = slotStart + column.widthFrac * slotExtent;
    int width = static_cast<int>(std::lround(slotEnd) - std::lround(slotStart)) - gap;
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
      const ColumnBleed bleed = columnBleed(m_columns[static_cast<size_t>(i)], *this);
      x += bleed.before + columnWidth(i, viewportPrimary) + gap + bleed.after;
    }
    if (columnIndex >= 0 && end < static_cast<int>(m_columns.size())) {
      // The column's own leading bleed. Presentation subtracts the same strut,
      // so the window's leading edge lands where the strip band starts.
      x += columnBleed(m_columns[static_cast<size_t>(end)], *this).before;
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
      const ColumnBleed bleed = columnBleed(m_columns[i], *this);
      total += bleed.before + columnWidth(static_cast<int>(i), viewportPrimary) + gap + bleed.after;
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
    column.widthFrac = m_config->scrolling.defaultExtentFraction.value_or(0.5);
    insertRow(column, 0, view, 1.0, 0.0);
    if (opensTabbed()) {
      column.tabs.form(0, 1, 0);
    }
    m_columns.insert(m_columns.begin() + index, std::move(column));
  }

  ScrollingLayout::JoinPoint ScrollingLayout::joinPoint(const Column& column) const {
    const std::optional<size_t> last =
        column.views.empty() ? std::nullopt : column.tabs.groupIndexAt(column.views.size() - 1);
    if (!last) {
      return {.row = column.views.size(), .group = std::nullopt};
    }
    const TabGroup& group = column.tabs.groups()[*last];
    const bool afterActive = m_config->tabs.newTabPosition == NewTabPosition::AfterActive;
    return {.row = afterActive ? group.active + 1 : group.end(), .group = last};
  }

  // Weight for a row about to be added at `row`, and the gap it takes over. A column keeps free space at its ends as
  // gap weight (pointer drags and secondary extent actions both put it there), and that free space is exactly where the
  // next row belongs, so the incoming row claims it instead of squeezing in beside it. Without a gap to claim the row
  // keeps `fallbackWeight`. The scaling keeps the other rows at their current pixel extents across the stack shrinking
  // by one inter-row gap.
  double ScrollingLayout::claimInsertWeight(Column& column, int row, double fallbackWeight) {
    ensureWeightCount(column);
    // Units, not rows: the stack gains one unit, and a tab group is one.
    const int existingRows = static_cast<int>(column.tabs.unitCount(column.views.size()));
    double edgeGapWeight = 0.0;
    bool consumesTopGap = false;
    bool consumesBottomGap = false;
    if (row == 0 && column.topGapWeight > 0.0) {
      edgeGapWeight = column.topGapWeight;
      consumesTopGap = true;
    } else if (row == static_cast<int>(column.views.size()) && column.bottomGapWeight > 0.0) {
      edgeGapWeight = column.bottomGapWeight;
      consumesBottomGap = true;
    }
    if (edgeGapWeight <= 0.0) {
      return fallbackWeight;
    }

    double insertedWeight = edgeGapWeight;
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
    return insertedWeight;
  }

  void ScrollingLayout::insertViewIntoColumn(View* view, int columnIndex, int rowIndex) {
    if (view == nullptr
        || columnOf(view) >= 0
        || columnIndex < 0
        || columnIndex >= static_cast<int>(m_columns.size())) {
      return;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    const int row = std::clamp(rowIndex, 0, static_cast<int>(column.views.size()));
    // A row landing strictly inside a tab group becomes one of its tabs and adds no unit to claim space for.
    const TabGroup* around = column.tabs.groupAt(static_cast<size_t>(row));
    const bool joins = around != nullptr && static_cast<size_t>(row) > around->first;
    const double insertedWeight = joins ? 1.0 : claimInsertWeight(column, row, 1.0);
    insertRow(column, static_cast<size_t>(row), view, insertedWeight, 0.0);
  }

  bool ScrollingLayout::insertTab(View* view, int columnIndex, int rowIndex, const View* member) {
    if (view == nullptr
        || columnOf(view) >= 0
        || columnIndex < 0
        || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    const TabGroup* group = tabGroupOf(column, member);
    if (group == nullptr || rowIndex < 0) {
      return false;
    }
    const auto row = static_cast<size_t>(rowIndex);
    if (row < group->first || row > group->end()) {
      return false;
    }
    insertRow(column, row, view, 1.0, 0.0, static_cast<size_t>(group - column.tabs.groups().data()));
    return true;
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
    // Remember width for later expel
    const double rememberedExtent = source.savedWidthFrac > 0 ? source.savedWidthFrac : source.widthFrac;
    const int row = rowOf(view);
    if (row < 0) {
      return false;
    }
    const ErasedRow erased = eraseRow(source, static_cast<size_t>(row));
    // Joining a tab group adds no unit, so only a row of its own claims the column's free space.
    const JoinPoint join = joinPoint(destination);
    const double insertedWeight = join.group
        ? erased.heightWeight
        : claimInsertWeight(destination, static_cast<int>(join.row), erased.heightWeight);
    insertRow(destination, join.row, view, insertedWeight, rememberedExtent, join.group);
    if (source.views.empty()) {
      m_columns.erase(m_columns.begin() + sourceColumn);
    }
    return true;
  }

  bool ScrollingLayout::consumeFrom(View* view, int direction) {
    int destinationColumn = columnOf(view);
    const int sourceColumn = destinationColumn + direction;
    if ((direction != -1 && direction != 1)
        || destinationColumn < 0
        || sourceColumn < 0
        || sourceColumn >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& source = m_columns[static_cast<size_t>(sourceColumn)];
    View* pulled = columnEntry(source);
    const int row = pulled != nullptr ? rowOf(pulled) : -1;
    if (row < 0) {
      return false;
    }
    // The row keeps the extent it was pulled from, so a later expel restores the column it came out of.
    const double rememberedExtent = source.savedWidthFrac > 0 ? source.savedWidthFrac : source.widthFrac;
    const ErasedRow erased = eraseRow(source, static_cast<size_t>(row));
    if (source.views.empty()) {
      m_columns.erase(m_columns.begin() + sourceColumn);
      if (sourceColumn < destinationColumn) {
        --destinationColumn;
      }
    }
    Column& destination = m_columns[static_cast<size_t>(destinationColumn)];
    // The focused row's place in the stack is the anchor: a tab takes the pulled window beside it as another tab, and
    // any other row takes it as the row directly below. Focus stays where it was either way.
    const int focusRow = rowOf(view);
    if (const std::optional<size_t> group = destination.tabs.groupIndexAt(static_cast<size_t>(focusRow))) {
      const TabGroup& tabs = destination.tabs.groups()[*group];
      const bool afterActive = m_config->tabs.newTabPosition == NewTabPosition::AfterActive;
      insertRow(
          destination, afterActive ? tabs.active + 1 : tabs.end(), pulled, erased.heightWeight, rememberedExtent, group
      );
      return true;
    }
    const auto landing = static_cast<size_t>(focusRow + 1);
    const double insertedWeight = claimInsertWeight(destination, static_cast<int>(landing), erased.heightWeight);
    insertRow(destination, landing, pulled, insertedWeight, rememberedExtent);
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
    const int row = rowOf(view);
    if (row < 0) {
      return false;
    }
    const ErasedRow erased = eraseRow(source, static_cast<size_t>(row));
    // A row consumed from its own column expels back to the extent it had there.
    Column column;
    column.widthFrac = erased.rememberedExtent > 0.0 ? erased.rememberedExtent
                                                     : m_config->scrolling.defaultExtentFraction.value_or(0.5);
    insertRow(column, 0, view, erased.heightWeight, 0.0);
    if (opensTabbed()) {
      column.tabs.form(0, 1, 0);
    }
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
    if (direction != -1 && direction != 1) {
      return false;
    }
    const auto at = static_cast<size_t>(row);
    const size_t start = col.tabs.unitStart(at);
    const size_t end = col.tabs.unitEnd(at);
    // A tab steps out of its group toward the move, keeping the group's extent as its own.
    if (end - start > 1) {
      const size_t edge = direction > 0 ? end - 1 : start;
      if (direction > 0) {
        rotateRows(col, at, at + 1, end);
      } else {
        rotateRows(col, start, at, at + 1);
      }
      col.tabs.select(edge);
      col.tabs.leave(edge);
      return true;
    }
    const bool down = direction > 0;
    if ((down && end >= col.views.size()) || (!down && start == 0)) {
      return false;
    }
    const size_t neighbor = down ? end : start - 1;
    // A window standing alone steps into a tab group it meets, as its nearest tab, and shows there.
    if (!col.tabs.groupAt(at)) {
      if (const std::optional<size_t> group = col.tabs.groupIndexAt(neighbor)) {
        col.tabs.join(at, *group);
        col.tabs.select(at);
        setUnitWeight(col, at, col.heightWeights[neighbor]);
        return true;
      }
    }
    // Otherwise the two units trade places, each keeping its extent.
    const size_t first = down ? start : col.tabs.unitStart(neighbor);
    const size_t middle = down ? end : start;
    const size_t last = down ? col.tabs.unitEnd(neighbor) : end;
    rotateRows(col, first, middle, last);
    col.tabs.unitsSwapped(first, middle, last);
    return true;
  }

  bool ScrollingLayout::moveTab(const View* view, int direction) {
    const int column = columnOf(view);
    if (column < 0 || (direction != -1 && direction != 1)) {
      return false;
    }
    Column& col = m_columns[static_cast<size_t>(column)];
    const auto row = static_cast<size_t>(rowOf(view));
    const TabGroup* group = col.tabs.groupAt(row);
    if (group == nullptr || (direction < 0 && row == group->first) || (direction > 0 && row + 1 >= group->end())) {
      return false;
    }
    swapRows(col, row, direction > 0 ? row + 1 : row - 1);
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
    if (firstColumn == secondColumn) {
      // Within one tab group the shown tab keeps following its view, as a row move does.
      m_columns[static_cast<size_t>(firstColumn)].tabs.rowsSwapped(
          static_cast<size_t>(firstRow), static_cast<size_t>(secondRow)
      );
    }
    std::swap(
        m_columns[static_cast<size_t>(firstColumn)].views[static_cast<size_t>(firstRow)],
        m_columns[static_cast<size_t>(secondColumn)].views[static_cast<size_t>(secondRow)]
    );
    std::swap(
        m_columns[static_cast<size_t>(firstColumn)].rememberedScrollingExtents[static_cast<size_t>(firstRow)],
        m_columns[static_cast<size_t>(secondColumn)].rememberedScrollingExtents[static_cast<size_t>(secondRow)]
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
    eraseRow(column, static_cast<size_t>(rowOf(view)));
    if (column.views.empty()) {
      m_columns.erase(m_columns.begin() + columnIndex);
      if (m_removedFocusedColumn == columnIndex) {
        m_pendingRemovalReevaluate = true;
      }
    } else if (m_removedFocusedColumn == columnIndex) {
      // A row left a stacked column standing, so no survivor inherits the side and the strip keeps its resting place.
      m_removedFocusedColumn = -1;
    }
    std::erase_if(m_targets, [view](const Target& target) { return target.view == view; });
  }

  // The dying column is still in the layout here, so a reveal now would judge a pair containing the column about to
  // disappear. Forgetting the side is all that buys; the detach judges the pair on the geometry the removal leaves.
  void ScrollingLayout::noteRemovalOfFocusedColumn(int columnIndex) {
    if (columnIndex < 0) {
      return;
    }
    m_removedFocusedColumn = columnIndex;
    m_focusSide = FocusSide::None;
    m_lastFocusedColumn = -1;
  }

  void ScrollingLayout::reevaluateAfterRemoval(int columnIndex, int viewportPrimary) {
    if (!m_pendingRemovalReevaluate) {
      return;
    }
    // One-shot even when focus passed to a floating survivor, so a later unrelated removal cannot replay it.
    m_pendingRemovalReevaluate = false;
    const int removed = std::exchange(m_removedFocusedColumn, -1);
    if (columnIndex < 0) {
      return;
    }
    // The survivor inherits the side the removed column was on, as a focus move off that column would have had.
    m_focusSide = removed > columnIndex ? FocusSide::FromRight : FocusSide::FromLeft;
    reevaluateColumn(columnIndex, viewportPrimary);
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
    m_policyCenteredRest = false;
  }

  bool ScrollingLayout::centerColumn(int columnIndex, int viewportPrimary) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size()) || viewportPrimary <= 0) {
      return false;
    }
    setScroll(centeredScroll(columnIndex, viewportPrimary), true);
    return true;
  }

  double ScrollingLayout::centeredScroll(int columnIndex, int viewportPrimary) const {
    return static_cast<double>(columnX(columnIndex, viewportPrimary))
        - (viewportPrimary - columnWidth(columnIndex, viewportPrimary)) / 2.0;
  }

  void ScrollingLayout::clampScroll(int viewportPrimary) {
    double low = 0.0;
    auto high = static_cast<double>(maxScroll(viewportPrimary));
    if (m_centeredRest && !m_columns.empty() && viewportPrimary > 0) {
      low = std::min(low, centeredScroll(0, viewportPrimary));
      high = std::max(high, centeredScroll(static_cast<int>(m_columns.size()) - 1, viewportPrimary));
    }
    m_scroll = std::clamp(m_scroll, low, high);
  }

  void ScrollingLayout::reconcileFocusedColumn(int columnIndex, int viewportPrimary) {
    if (alwaysCentersFocus()) {
      centerColumn(columnIndex, viewportPrimary);
      return;
    }
    m_centeredRest = false;
    ensureVisible(columnIndex, viewportPrimary);
  }

  bool ScrollingLayout::alwaysCentersFocus() const {
    return m_config->scrolling.centerFocused == CenterFocusedColumn::Always;
  }

  bool ScrollingLayout::shouldCenterFocusedColumn(int columnIndex, int viewportPrimary, FocusSide side) const {
    switch (m_config->scrolling.centerFocused) {
    case CenterFocusedColumn::Always:
      return true;
    case CenterFocusedColumn::OnOverflow:
      return shouldCenterOnOverflow(columnIndex, viewportPrimary, side);
    case CenterFocusedColumn::Never:
      break;
    }
    return false;
  }

  ScrollingLayout::FocusSide ScrollingLayout::focusSideFrom(int columnIndex) const {
    if (m_lastFocusedColumn < 0
        || m_lastFocusedColumn >= static_cast<int>(m_columns.size())
        || m_lastFocusedColumn == columnIndex) {
      return FocusSide::None;
    }
    return m_lastFocusedColumn > columnIndex ? FocusSide::FromRight : FocusSide::FromLeft;
  }

  // Centers the column focus is moving to when it and the column on the side focus came from cannot share the
  // viewport. The reference is the immediate neighbor, not the previously focused column, so a jump across the strip
  // is judged by the same pair spacing as a step.
  bool ScrollingLayout::shouldCenterOnOverflow(int columnIndex, int viewportPrimary, FocusSide side) const {
    const int columnCount = static_cast<int>(m_columns.size());
    if (columnIndex < 0 || columnIndex >= columnCount || side == FocusSide::None) {
      return false;
    }
    const int neighbor = focusNeighbor(columnIndex, side);
    if (neighbor == columnIndex) {
      return false;
    }
    // Leading edge of the first column to trailing edge of the second, so the pair's own widths both count.
    const int first = std::min(columnIndex, neighbor);
    const int last = std::max(columnIndex, neighbor);
    const int span =
        columnX(last, viewportPrimary) - columnX(first, viewportPrimary) + columnWidth(last, viewportPrimary);
    return span > viewportPrimary;
  }

  int ScrollingLayout::focusNeighbor(int columnIndex, FocusSide side) const {
    const int columnCount = static_cast<int>(m_columns.size());
    return side == FocusSide::FromRight ? std::min(columnIndex + 1, columnCount - 1) : std::max(columnIndex - 1, 0);
  }

  double ScrollingLayout::pairScroll(int columnIndex, int viewportPrimary, int neighbor) const {
    const double max = static_cast<double>(std::max(0, totalWidth(viewportPrimary) - viewportPrimary));
    const int x = columnX(neighbor, viewportPrimary);
    const double edge = neighbor < columnIndex
        ? static_cast<double>(x)
        : static_cast<double>(x + columnWidth(neighbor, viewportPrimary)) - static_cast<double>(viewportPrimary);
    return std::clamp(edge, 0.0, max);
  }

  double
  ScrollingLayout::targetScrollForEnsureVisible(int columnIndex, int viewportPrimary, bool center, bool force) const {
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
    if (center) {
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
    const bool centered = shouldCenterFocusedColumn(columnIndex, viewportPrimary, focusSideFrom(columnIndex));
    return std::abs(targetScrollForEnsureVisible(columnIndex, viewportPrimary, centered) - m_scroll)
        / static_cast<double>(viewportPrimary);
  }

  double ScrollingLayout::scrollShiftForColumnRemoval(int columnIndex, int viewportPrimary) const {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size()) || viewportPrimary <= 0) {
      return 0.0;
    }
    // A stack losing one row leaves the horizontal geometry alone; only the
    // last view out takes the column, and the space, with it.
    const Column& column = m_columns[static_cast<size_t>(columnIndex)];
    if (column.views.size() != 1) {
      return 0.0;
    }
    // The lane frees its bleed on both sides too. columnX() already includes the
    // leading one, so measure the hidden extent from where that bleed starts.
    const ColumnBleed bleed = columnBleed(column, *this);
    const auto span = static_cast<double>(
        bleed.before + columnWidth(columnIndex, viewportPrimary) + m_config->totalGap + bleed.after
    );
    const double hidden = m_scroll - static_cast<double>(columnX(columnIndex, viewportPrimary) - bleed.before);
    return std::clamp(hidden, 0.0, span);
  }

  void ScrollingLayout::revealColumn(int columnIndex, int viewportPrimary, bool center, bool force) {
    const double target = targetScrollForEnsureVisible(columnIndex, viewportPrimary, center, force);
    m_centeredRest = center || (m_centeredRest && target == m_scroll);
    // A rest the user asked for with column-center outlives a re-judgment, so keep track of who asked for this one.
    m_policyCenteredRest = m_centeredRest && (center || m_policyCenteredRest);
    m_scroll = target;
  }

  void ScrollingLayout::ensureVisible(int columnIndex, int viewportPrimary) {
    revealColumn(columnIndex, viewportPrimary, alwaysCentersFocus());
  }

  // `previousIndex` is where the column sat before it moved, which is the side focus came from. A move keeps focus, so
  // OnOverflow cannot read an index that no longer holds the column. -1 derives the side from the last focused column.
  void ScrollingLayout::activateColumn(int columnIndex, int viewportPrimary, int previousIndex) {
    if (previousIndex >= 0 && previousIndex < static_cast<int>(m_columns.size()) && previousIndex != columnIndex) {
      m_lastFocusedColumn = previousIndex;
    }
    m_focusSide = focusSideFrom(columnIndex);
    revealColumn(columnIndex, viewportPrimary, shouldCenterFocusedColumn(columnIndex, viewportPrimary, m_focusSide));
    if (columnIndex >= 0) {
      m_lastFocusedColumn = columnIndex;
    }
  }

  void ScrollingLayout::reevaluateColumn(int columnIndex, int viewportPrimary) {
    applyCenteringPolicy(columnIndex, viewportPrimary, false);
    if (columnIndex >= 0 && columnIndex < static_cast<int>(m_columns.size())) {
      m_lastFocusedColumn = columnIndex;
    }
  }

  // The pair is the one the last activation used, focus having not moved since; with none to remember, the column after
  // it stands in for the missing side.
  ScrollingLayout::FocusSide ScrollingLayout::reevaluationSide(int columnIndex) const {
    if (m_focusSide != FocusSide::None) {
      return m_focusSide;
    }
    const int columnCount = static_cast<int>(m_columns.size());
    return columnIndex + 1 < columnCount ? FocusSide::FromRight : FocusSide::FromLeft;
  }

  // `force` is snapVisible's: an extent change can leave the column parked where an ordinary reveal calls it visible.
  void ScrollingLayout::applyCenteringPolicy(int columnIndex, int viewportPrimary, bool force) {
    const int columnCount = static_cast<int>(m_columns.size());
    if (columnIndex < 0 || columnIndex >= columnCount) {
      return;
    }
    const FocusSide side = reevaluationSide(columnIndex);
    if (shouldCenterFocusedColumn(columnIndex, viewportPrimary, side)) {
      revealColumn(columnIndex, viewportPrimary, true);
      // Always centers by the user's standing choice, so only OnOverflow's centering is the policy's to take back.
      m_policyCenteredRest = m_config->scrolling.centerFocused == CenterFocusedColumn::OnOverflow;
    } else if (m_config->scrolling.centerFocused == CenterFocusedColumn::OnOverflow && m_policyCenteredRest) {
      // The pair fits again, so a centering the policy made is stale. A plain fit would keep it, the column being fully
      // visible already: put the pair back at the edge it reads from, where the focus move left the strip.
      m_centeredRest = false;
      m_policyCenteredRest = false;
      m_scroll = pairScroll(columnIndex, viewportPrimary, focusNeighbor(columnIndex, side));
    } else {
      revealColumn(columnIndex, viewportPrimary, false, force);
    }
  }

  // A fullscreen or maximize-to-edges transition is a width change, so the snap judges it instead of only revealing it.
  void ScrollingLayout::snapVisible(int columnIndex, int viewportPrimary) {
    applyCenteringPolicy(columnIndex, viewportPrimary, true);
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
      const ColumnBleed bleed = columnBleed(column, *this);
      runningColumnX += bleed.before;
      const int primarySize = columnWidth(static_cast<int>(columnIndex), viewportPrimary);
      if (column.views.empty()) {
        runningColumnX += primarySize + gap + bleed.after;
        continue;
      }
      ensureWeightCount(column);
      const int primary =
          (v ? usable.y : usable.x) + edgePad + runningColumnX - static_cast<int>(std::lround(m_scroll));
      runningColumnX += primarySize + gap + bleed.after;
      // Rows stack as units: a window standing alone, or a tab group whose tabs all take the group's one share.
      const int rowCount = static_cast<int>(column.tabs.unitCount(column.views.size()));
      const int gapsTotal = std::max(0, rowCount - 1) * gap;
      const int stackCross = std::max(rowCount, availableCross - gapsTotal);
      const double totalWeight = columnTotalWeight(column);

      int cross = (v ? usable.x : usable.y) + edgePad;
      const int startGapPx =
          static_cast<int>(std::lround(std::max(0.0, column.topGapWeight) / totalWeight * stackCross));
      cross += startGapPx;
      int used = startGapPx;

      for (size_t start = 0; start < column.views.size(); start = column.tabs.unitEnd(start)) {
        const double weight = std::max(kMinHeightWeight, column.heightWeights[start]);
        int crossSize = static_cast<int>(std::lround(weight / totalWeight * stackCross));
        if (column.tabs.unitEnd(start) >= column.views.size()) {
          const int endGapPx =
              static_cast<int>(std::lround(std::max(0.0, column.bottomGapWeight) / totalWeight * stackCross));
          crossSize = std::max(1, stackCross - used - endGapPx);
        } else {
          crossSize = std::max(1, crossSize);
        }
        if (const TabGroup* group = column.tabs.groupAt(start)) {
          const wlr_box unit = v ? wlr_box{.x = cross, .y = primary, .width = crossSize, .height = primarySize}
                                 : wlr_box{.x = primary, .y = cross, .width = primarySize, .height = crossSize};
          pushTabTargets(column, *group, unit);
        } else {
          View* view = column.views[start];
          if (view != nullptr) {
            const LayoutConstraints constraints = constraintsFor(view);
            crossSize = v ? constraints.clampWidth(crossSize) : constraints.clampHeight(crossSize);
          }
          if (v) {
            m_targets.push_back({.view = view, .x = cross, .y = primary, .width = crossSize, .height = primarySize});
          } else {
            m_targets.push_back({.view = view, .x = primary, .y = cross, .width = primarySize, .height = crossSize});
          }
        }
        cross += crossSize + gap;
        used += crossSize;
      }
    }
  }

  void ScrollingLayout::pushTabTargets(const Column& column, const TabGroup& group, const wlr_box& unit) {
    // Every tab gets the group's unit less its bar.
    const bool v = vertical();
    const wlr_box shared = tabbedBox(unit, group.count, group.bar);
    for (size_t row = group.first; row < group.end() && row < column.views.size(); ++row) {
      View* view = column.views[row];
      const LayoutConstraints constraints = constraintsFor(view);
      // The primary extent comes from the lane, already clamped for every member; only the cross extent is the view's.
      const int width = v ? constraints.clampWidth(shared.width) : shared.width;
      const int height = v ? shared.height : constraints.clampHeight(shared.height);
      m_targets.push_back({.view = view, .x = shared.x, .y = shared.y, .width = width, .height = height});
    }
  }

  bool ScrollingLayout::setTabbed(const View* view, bool tabbed) {
    const int columnIndex = columnOf(view);
    if (columnIndex < 0) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    ensureWeightCount(column);
    const auto row = static_cast<size_t>(rowOf(view));
    if (!tabbed) {
      const TabGroup* group = column.tabs.groupAt(row);
      if (group == nullptr) {
        return false;
      }
      // The tabs split the group's extent evenly, so the column keeps its other rows where they were.
      const size_t first = group->first;
      const size_t count = group->count;
      const double share = std::max(kMinHeightWeight, column.heightWeights[first]) / static_cast<double>(count);
      column.tabs.dissolve(row);
      for (size_t member = first; member < first + count; ++member) {
        column.heightWeights[member] = share;
      }
      return true;
    }
    if (column.tabs.groupAt(row) != nullptr) {
      return false;
    }
    // The run of rows standing alone around `view`, as far as the nearest tab groups, becomes one group showing it,
    // taking the extent the run had.
    size_t first = row;
    while (first > 0 && column.tabs.groupAt(first - 1) == nullptr) {
      --first;
    }
    size_t end = row + 1;
    while (end < column.views.size() && column.tabs.groupAt(end) == nullptr) {
      ++end;
    }
    double extent = 0.0;
    for (size_t member = first; member < end; ++member) {
      extent += std::max(kMinHeightWeight, column.heightWeights[member]);
    }
    column.tabs.form(first, end - first, row);
    setUnitWeight(column, row, extent);
    return true;
  }

  bool ScrollingLayout::setTabBar(const View* view, std::optional<bool> shown) {
    const int columnIndex = columnOf(view);
    if (columnIndex < 0) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    const auto row = static_cast<size_t>(rowOf(view));
    const TabGroup* group = column.tabs.groupAt(row);
    if (group == nullptr) {
      return false;
    }
    const bool current = tabBarShown(*m_config, group->count, group->bar);
    const bool target = shown.value_or(!current);
    column.tabs.setBar(row, target);
    return target != current;
  }

  bool ScrollingLayout::adoptTabs(int columnIndex, const ColumnTabs& tabs) {
    if (columnIndex < 0 || columnIndex >= static_cast<int>(m_columns.size())) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    if (!tabs.empty() && tabs.groups().back().end() > column.views.size()) {
      return false;
    }
    column.tabs = tabs;
    return true;
  }

  bool ScrollingLayout::selectTab(const View* view) {
    const int columnIndex = columnOf(view);
    if (columnIndex < 0) {
      return false;
    }
    return m_columns[static_cast<size_t>(columnIndex)].tabs.select(static_cast<size_t>(rowOf(view)));
  }

  Layout::InitialSize ScrollingLayout::initialSize(
      const wlr_box& usable, bool wantMaximized, std::optional<double> ruleExtent, std::optional<int> ruleExtentPx,
      const View* /*splitAnchor*/
  ) const {
    const wlr_box content = contentArea(usable);
    const int viewportPrimary = vertical() ? content.height : content.width;
    int extent = 0;
    if (wantMaximized) {
      extent = viewportPrimary;
    } else if (ruleExtentPx) {
      extent = std::clamp(*ruleExtentPx, 1, viewportPrimary);
    } else if (ruleExtent) {
      extent = fractionalWidth(viewportPrimary, *ruleExtent);
    } else if (m_config->scrolling.defaultExtentFraction) {
      extent = fractionalWidth(viewportPrimary, *m_config->scrolling.defaultExtentFraction);
    }
    // A column that opens tabbed gives its bar the space its window would otherwise take, on whichever side the bar is.
    // A primary extent of 0 leaves that size to the client, and stays 0.
    const bool v = vertical();
    const wlr_box lane = v ? wlr_box{.x = 0, .y = 0, .width = content.width, .height = std::max(1, extent)}
                           : wlr_box{.x = 0, .y = 0, .width = std::max(1, extent), .height = content.height};
    const wlr_box shared = opensTabbed() ? tabbedBox(lane, 1, std::nullopt) : lane;
    if (v) {
      return {.width = shared.width, .height = extent > 0 ? shared.height : 0};
    }
    return {.width = extent > 0 ? shared.width : 0, .height = shared.height};
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
    column.widthFrac = nextFractionPreset(m_config->extentPresets, column.widthFrac, direction);
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
      return m_config->scrolling.defaultExtentFraction.value_or(0.5);
    }
    return m_columns[static_cast<size_t>(columnIndex)].widthFrac;
  }

  double ScrollingLayout::heightFraction(const View* view) const {
    const int columnIndex = columnOf(view);
    if (columnIndex < 0) {
      return 1.0;
    }
    const Column& column = m_columns[static_cast<size_t>(columnIndex)];
    const int row = rowOf(view);
    // A lone row is not automatically full height: the edge gaps it can be dragged away from count towards the
    // column's weight, so the same weight-over-total ratio describes solo and stacked rows alike.
    return std::max(kMinHeightWeight, heightWeight(columnIndex, row)) / columnTotalWeight(column);
  }

  bool ScrollingLayout::setHeightFraction(View* view, double fraction) {
    const int columnIndex = columnOf(view);
    if (columnIndex < 0) {
      return false;
    }
    Column& column = m_columns[static_cast<size_t>(columnIndex)];
    const int row = rowOf(view);
    // A tab sizes its whole group, the unit it shows in.
    if (column.tabs.unitCount(column.views.size()) == 1) {
      // No sibling row to trade weight with, so the remainder goes to the column's edge gaps, exactly as dragging the
      // window's top or bottom edge does. An ungapped window keeps its top edge and frees the space below, which is
      // where the next window in the column lands. Existing gaps keep their proportion, so a window a drag pushed
      // against one end stays there.
      ensureWeightCount(column);
      const double total = columnTotalWeight(column);
      const double target = std::clamp(fraction, 0.1, 1.0);
      const double remainder = total * (1.0 - target);
      const double topGap = std::max(0.0, column.topGapWeight);
      const double gaps = topGap + std::max(0.0, column.bottomGapWeight);
      const double topShare = gaps > 0.0 ? topGap / gaps : 0.0;
      column.topGapWeight = remainder * topShare;
      column.bottomGapWeight = remainder - column.topGapWeight;
      return setHeightWeight(columnIndex, row, total * target);
    }
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
    // The boundary below the unit holding `upperRow`, which the next unit shares.
    const size_t lowerRow = upperRow < 0 ? column.views.size() : column.tabs.unitEnd(static_cast<size_t>(upperRow));
    if (lowerRow >= column.views.size()) {
      return false;
    }
    setUnitWeight(column, static_cast<size_t>(upperRow), std::max(kMinHeightWeight, upperWeight));
    setUnitWeight(column, lowerRow, std::max(kMinHeightWeight, lowerWeight));
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
    setUnitWeight(column, static_cast<size_t>(row), std::max(kMinHeightWeight, weight));
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
          int startStripPrimaryPx, int upperRow, int lowerRow, double startUpperWeight, double startLowerWeight
      )
          : m_layout(layout), m_column(column), m_row(row), m_edges(edges), m_vertical(vertical),
            m_soloPrimary(soloPrimary), m_clearedFullWidth(clearedFullWidth), m_startScroll(startScroll),
            m_startColumnX(startColumnX), m_startPrimaryPx(startPrimaryPx), m_startPrevPrimaryPx(startPrevPrimaryPx),
            m_startStripPrimaryPx(startStripPrimaryPx), m_upperRow(upperRow), m_lowerRow(lowerRow),
            m_startUpperWeight(startUpperWeight), m_startLowerWeight(startLowerWeight) {}

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
            const int requestedPrev = m_startPrevPrimaryPx + static_cast<int>(std::lround(dPrimary));
            const int minimumPrev = std::max(minPrev, pair - gap - maxCur);
            const int maximumPrev = std::min(maxPrev, pair - gap - minCur);
            const int newPrev = std::clamp(requestedPrev, minimumPrev, maximumPrev);
            int newCur = pair - gap - newPrev;
            const bool growsPastPreviousMinimum = requestedPrev < minimumPrev && minimumPrev == minPrev;
            if (growsPastPreviousMinimum) {
              newCur = std::min(m_startPrimaryPx - static_cast<int>(std::lround(dPrimary)), maxCur);
            }
            setColumnPrimaryPx(m_column - 1, newPrev);
            setColumnPrimaryPx(m_column, newCur);
            if (growsPastPreviousMinimum) {
              layout.setScroll(
                  m_startScroll
                      + static_cast<double>(layout.columnX(m_column, viewportPrimary) - m_startColumnX)
                      + static_cast<double>(newCur - m_startPrimaryPx),
                  true
              );
            }
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
          // A tab group is one unit of the stack, so the stack counts units and the boundary sits between units.
          const int rowCount = static_cast<int>(column.tabs.unitCount(column.views.size()));
          const int gapsTotal = std::max(0, rowCount - 1) * gap;
          const int stackCross = std::max(rowCount, availableCross - gapsTotal);
          if (stackCross > 0) {
            constexpr double kMinWindow = 0.05;
            double totalWeight = std::max(0.0, column.topGapWeight) + std::max(0.0, column.bottomGapWeight);
            for (size_t row = 0; row < column.heightWeights.size(); row = column.tabs.unitEnd(row)) {
              totalWeight += std::max(kMinWindow, column.heightWeights[row]);
            }
            totalWeight = std::max(kMinWindow, totalWeight);

            auto minWindowWeight = [&](int row) {
              const int minimum = unitMinCross(column, static_cast<size_t>(row), layout, m_vertical);
              return std::max(kMinWindow, static_cast<double>(minimum) / stackCross * totalWeight);
            };

            const double pair = std::max(kMinWindow, m_startUpperWeight + m_startLowerWeight);
            const double deltaWeight = dCross / static_cast<double>(stackCross) * totalWeight;

            auto splitWindows = [&](double startUpper, double delta, double minUpper, double minLower) {
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
            auto splitGapAndWindow = [&](double startGap, double deltaGap, double minWindow) {
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

            // "Upper" denotes the cross-start side, which is left when vertical. A missing upper unit is the column's
            // start gap, a missing lower one its end gap.
            if (m_upperRow < 0 && m_lowerRow >= 0) {
              const auto [gapWeight, windowWeight] =
                  splitGapAndWindow(m_startUpperWeight, deltaWeight, minWindowWeight(m_lowerRow));
              layout.setTopGapWeight(m_column, gapWeight);
              layout.setHeightWeight(m_column, m_lowerRow, windowWeight);
            } else if (m_lowerRow < 0 && m_upperRow >= 0) {
              const auto [gapWeight, windowWeight] =
                  splitGapAndWindow(m_startLowerWeight, -deltaWeight, minWindowWeight(m_upperRow));
              layout.setHeightWeight(m_column, m_upperRow, windowWeight);
              layout.setBottomGapWeight(m_column, gapWeight);
            } else if (m_upperRow >= 0 && m_lowerRow >= 0) {
              const auto [upper, lower] = splitWindows(
                  m_startUpperWeight, deltaWeight, minWindowWeight(m_upperRow), minWindowWeight(m_lowerRow)
              );
              layout.setRowBoundary(m_column, m_upperRow, upper, lower);
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
      // First rows of the units either side of the dragged boundary; -1 for the column's edge gap there.
      int m_upperRow;
      int m_lowerRow;
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
    const int columnIndex = columnOf(view);
    if (columnIndex == 0 && !m_config->scrolling.centerUnderfullStrip) {
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

    const int startColumnX = columnX(column, viewportPrimary);
    // The column's own extent, not the window's: a tab group's bar can take part of it.
    const int startPrimaryPx = columnWidth(column, viewportPrimary);
    if (startPrimaryPx >= viewportPrimary) {
      soloPrimary = true;
    }
    int startPrevPrimaryPx = 0;
    if (column > 0 && !soloPrimary) {
      startPrevPrimaryPx = columnWidth(column - 1, viewportPrimary);
    }
    const double startScroll = scroll();

    // The boundary dragged lies between the unit holding the window, which may be a tab group, and the unit or edge gap
    // beside it.
    int upperRow = -1;
    int lowerRow = -1;
    double startUpperWeight = 0;
    double startLowerWeight = 0;
    if ((edges & (crossStartEdge | crossEndEdge)) != 0 && row >= 0) {
      const Column& lane = m_columns[static_cast<size_t>(column)];
      const size_t start = lane.tabs.unitStart(static_cast<size_t>(row));
      const size_t end = lane.tabs.unitEnd(static_cast<size_t>(row));
      if ((edges & crossStartEdge) != 0) {
        lowerRow = static_cast<int>(start);
        upperRow = start > 0 ? static_cast<int>(lane.tabs.unitStart(start - 1)) : -1;
        startUpperWeight = upperRow >= 0 ? heightWeight(column, upperRow) : topGapWeight(column);
        startLowerWeight = heightWeight(column, lowerRow);
      } else {
        upperRow = static_cast<int>(start);
        lowerRow = end < lane.views.size() ? static_cast<int>(end) : -1;
        startUpperWeight = heightWeight(column, upperRow);
        startLowerWeight = lowerRow >= 0 ? heightWeight(column, lowerRow) : bottomGapWeight(column);
      }
    }

    return std::make_unique<ScrollingResizeGrab>(
        this, column, row, edges, v, soloPrimary, clearedFullWidth, startScroll, startColumnX, startPrimaryPx,
        startPrevPrimaryPx, rawTotalWidth(viewportPrimary), upperRow, lowerRow, startUpperWeight, startLowerWeight
    );
  }

} // namespace umbriel
